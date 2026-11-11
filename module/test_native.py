"""Run adaptive resolution and patch-safety tests in standalone Android processes."""
from pathlib import Path
import os,subprocess,sys
H=Path(__file__).resolve().parent;ROOT=H.parent;REFS=Path(os.environ.get('HYPERGLASS_REFERENCE_DIR',ROOT/'fixtures/reference'));B=H/'.build'
sys.path.insert(0,str(ROOT))
from tools.build_env import TOOLCHAIN as T
if len(sys.argv)!=2:raise SystemExit('Usage: .venv/bin/python module/test_native.py ADB_SERIAL')
ADB=['adb','-s',sys.argv[1]];REMOTE='/data/local/tmp/hyperglass-adaptive-tests';OUT=B/'native-tests';OUT.mkdir(exist_ok=True)
def run(*args,**kw):return subprocess.run(list(map(str,args)),check=True,**kw)
def compile_test(name,cpp,objects=()):
 compiler=T/('aarch64-linux-android35-clang++'if cpp else'aarch64-linux-android35-clang')
 source=H/('native/'+name+('.cpp'if cpp else'.c'));binary=OUT/name
 run(compiler,'-std=c++17'if cpp else'-std=c11','-O2','-Wall','-Wextra','-Werror',*(['-static-libstdc++']if cpp else[]),'-I'+str(H/'native'),'-I'+str(B),source,*objects,'-llog','-ldl','-o',binary)
 return binary
bins=[compile_test('test_runtime',False,[B/'assets.o']),compile_test('test_runtime_tick',True),compile_test('test_page_guard',True),compile_test('test_material',True,[B/'elf_image.o',*sorted(B.glob('xz_*.o'))]),compile_test('test_resolver',True,[B/'elf_image.o',B/'resolver.o',*sorted(B.glob('xz_*.o'))])]
old=REFS/'launcher-801027722/libapp.so';new=REFS/'launcher-801029226/MiuiHome.apk'
fixtures={}
run(sys.executable,H/'adaptive/make_future_fixture.py',old,OUT/'future.so');fixtures['future']=OUT/'future.so'
for mode in ['core-layout','widget-layout','ambiguous']:
 run(sys.executable,H/'adaptive/make_failure_fixture.py',old,mode,OUT/(mode+'.so'));fixtures[mode]=OUT/(mode+'.so')
engine=REFS/'launcher-801027722/libhyper_os_flutter.so'
run(sys.executable,H/'adaptive/make_failure_fixture.py',engine,'engine-layout',OUT/'engine-layout.so')
run(*ADB,'shell','mkdir','-p',REMOTE)
try:
 for binary in bins:run(*ADB,'push',binary,REMOTE+'/'+binary.name)
 run(*ADB,'shell',REMOTE+'/test_runtime');run(*ADB,'shell',REMOTE+'/test_runtime_tick');run(*ADB,'shell',REMOTE+'/test_page_guard');run(*ADB,'shell',REMOTE+'/test_material')
 inputs=[('7722',old,0,23,''),('9226',new,0,23,'!/lib/arm64-v8a/libapp.so'),('engine',REFS/'launcher-801027722/libhyper_os_flutter.so',1,2,''),('maml',REFS/'launcher-801027722/librust_maml_sdk.so',2,8,'')]
 inputs.extend((name,file,0,21 if name in ['core-layout','ambiguous']else 3 if name=='widget-layout'else 23,'')for name,file in fixtures.items())
 inputs.append(('engine-layout',OUT/'engine-layout.so',1,0,''))
 for name,file,index,ready,suffix in inputs:
  target=REMOTE+'/'+name+file.suffix;plan=REMOTE+'/'+name+'.json'
  run(*ADB,'push',file,target);run(*ADB,'shell',f'HG_EXPECT_READY={ready}',REMOTE+'/test_resolver',index,target+suffix,plan)
  run(*ADB,'pull',plan,OUT/(name+'.json'))
  if name in ['core-layout','widget-layout','ambiguous']:
   import json
   data=json.loads((OUT/(name+'.json')).read_text());disabled=20 if name=='widget-layout'else 2
   assert all(not(group&disabled)for _,_,group in data['patches']),name
  if name=='engine-layout':
   import json
   assert json.loads((OUT/(name+'.json')).read_text())['patches']==[]
 run(sys.executable,H/'adaptive/test_runtime_plans.py',OUT/'7722.json',old,OUT/'9226.json',REFS/'launcher-801029226/libapp.so',OUT/'future.json',OUT/'future.so')
finally:run(*ADB,'shell','rm','-rf',REMOTE)
print('PASS native concurrency, page guard, old/new/APK sources, unseen layout changes and isolated feature failures')
