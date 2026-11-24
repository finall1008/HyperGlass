"""Run adaptive resolution and patch-safety tests in standalone Android processes."""
from pathlib import Path
import json,subprocess,sys
H=Path(__file__).resolve().parent;ROOT=H.parent;B=H/'.build'
sys.path.insert(0,str(ROOT))
from tools.build_env import TOOLCHAIN as T
from test_cases import check_plan,report_compatibility,required_cases,run,runtime_plan_arguments
if len(sys.argv)!=2:raise SystemExit('Usage: .venv/bin/python module/test_native.py ADB_SERIAL')
ADB=['timeout','45','adb','-s',sys.argv[1]];REMOTE='/data/local/tmp/hyperglass-adaptive-tests';OUT=B/'native-tests';OUT.mkdir(exist_ok=True)
def compile_test(name,cpp,objects=()):
 compiler=T/('aarch64-linux-android35-clang++'if cpp else'aarch64-linux-android35-clang')
 source=H/('native/'+name+('.cpp'if cpp else'.c'));binary=OUT/name
 run(compiler,'-std=c++17'if cpp else'-std=c11','-O2','-Wall','-Wextra','-Werror',*(['-static-libstdc++']if cpp else[]),'-I'+str(H/'native'),'-I'+str(B),source,*objects,'-llog','-ldl','-o',binary)
 return binary
bins=[compile_test('test_runtime',False,[B/'assets.o']),compile_test('test_runtime_tick',True),compile_test('test_page_guard',True),compile_test('test_material',True,[B/'elf_image.o',*sorted(B.glob('xz_*.o'))]),compile_test('test_resolver',True,[B/'elf_image.o',B/'resolver.o',*sorted(B.glob('xz_*.o'))])]
cases=required_cases(OUT)
run(*ADB,'shell','mkdir','-p',REMOTE)
try:
 for binary in bins:run(*ADB,'push',binary,REMOTE+'/'+binary.name)
 for test in ['test_runtime','test_runtime_tick','test_page_guard','test_material']:run(*ADB,'shell',REMOTE+'/'+test)
 for name,file,index,ready,suffix in cases:
  target=REMOTE+'/'+name+file.suffix;plan=REMOTE+'/'+name+'.json'
  run(*ADB,'push',file,target);run(*ADB,'shell',f'HG_EXPECT_READY={ready}',REMOTE+'/test_resolver',index,target+suffix,plan)
  run(*ADB,'pull',plan,OUT/(name+'.json'))
  check_plan(name,json.loads((OUT/(name+'.json')).read_text()))
 def resolve(file,suffix):
  target=REMOTE+'/compat'+file.suffix;run(*ADB,'push',file,target)
  return subprocess.run([*ADB,'shell',REMOTE+'/test_resolver','0',target+suffix],capture_output=True,text=True).stdout
 report_compatibility(resolve)
 run(sys.executable,H/'adaptive/test_runtime_plans.py',*runtime_plan_arguments(OUT))
finally:run(*ADB,'shell','rm','-rf',REMOTE)
print('PASS native concurrency, page guard, reference ELF/APK, unseen layout changes and isolated feature failures')
