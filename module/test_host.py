"""Run the native ELF resolver and execute its relocated ARM64 plans on Linux."""
from pathlib import Path
import json
import os
import subprocess
import sys
H=Path(__file__).resolve().parent;ROOT=H.parent;B=H/'.build';OUT=B/'host-tests';OUT.mkdir(exist_ok=True)
REFS=Path(os.environ.get('HYPERGLASS_REFERENCE_DIR',ROOT/'fixtures/reference'))
cc=os.environ.get('CC','cc');cxx=os.environ.get('CXX','c++')
def run(*arguments,**options):return subprocess.run(list(map(str,arguments)),check=True,**options)
objects=[]
for source in sorted((H/'third_party/xz').glob('*.c')):
    output=OUT/(source.stem+'.o');run(cc,'-O2','-DXZ_USE_CRC64','-DXZ_DEC_ARM64','-I'+str(H/'third_party/xz'),'-c',source,'-o',output);objects.append(output)
binary=OUT/'test_resolver'
run(cxx,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-DXZ_USE_CRC64','-I'+str(H/'native'),'-I'+str(H/'third_party/xz'),'-I'+str(B),H/'native/test_resolver.cpp',H/'native/resolver.cpp',H/'native/elf_image.cpp',*objects,'-pthread','-o',binary)
old=REFS/'launcher-801027722/libapp.so';new=REFS/'launcher-801029226/MiuiHome.apk';engine=REFS/'launcher-801027722/libhyper_os_flutter.so'
run(sys.executable,H/'adaptive/make_future_fixture.py',old,OUT/'future.so')
for mode in ['core-layout','widget-layout','ambiguous']:run(sys.executable,H/'adaptive/make_failure_fixture.py',old,mode,OUT/(mode+'.so'))
run(sys.executable,H/'adaptive/make_failure_fixture.py',engine,'engine-layout',OUT/'engine-layout.so')
inputs=[('7722',old,0,23,''),('9226',new,0,23,'!/lib/arm64-v8a/libapp.so'),('engine',engine,1,2,''),('maml',REFS/'launcher-801027722/librust_maml_sdk.so',2,8,''),('future',OUT/'future.so',0,23,''),('core-layout',OUT/'core-layout.so',0,21,''),('widget-layout',OUT/'widget-layout.so',0,3,''),('ambiguous',OUT/'ambiguous.so',0,21,''),('engine-layout',OUT/'engine-layout.so',1,0,'')]
for name,file,index,ready,suffix in inputs:
    plan=OUT/(name+'.json');run(binary,index,str(file)+suffix,plan,env=dict(os.environ,HG_EXPECT_READY=str(ready)))
    parsed=json.loads(plan.read_text());assert parsed['ready']==ready
    if name in ['core-layout','widget-layout','ambiguous']:
        disabled=20 if name=='widget-layout'else 2
        assert all(not(group&disabled)for _,_,group in parsed['patches']),name
    if name=='engine-layout':assert parsed['patches']==[]
run(sys.executable,H/'adaptive/test_runtime_plans.py',OUT/'7722.json',old,OUT/'9226.json',REFS/'launcher-801029226/libapp.so',OUT/'future.json',OUT/'future.so')
print('PASS host native resolver, old/new/APK fixtures, changed layouts, isolated failures and relocated ARM64 execution')
