"""Run the native ELF resolver and execute its relocated ARM64 plans on Linux."""
from pathlib import Path
import json
import os
import subprocess
import sys
from test_cases import check_plan, report_compatibility, required_cases, run, runtime_plan_arguments
H=Path(__file__).resolve().parent;B=H/'.build';OUT=B/'host-tests';OUT.mkdir(parents=True,exist_ok=True)
cc=os.environ.get('CC','cc');cxx=os.environ.get('CXX','c++')
objects=[]
for source in sorted((H/'third_party/xz').glob('*.c')):
    output=OUT/(source.stem+'.o');run(cc,'-O2','-DXZ_USE_CRC64','-DXZ_DEC_ARM64','-I'+str(H/'third_party/xz'),'-c',source,'-o',output);objects.append(output)
binary=OUT/'test_resolver'
run(cxx,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-DXZ_USE_CRC64','-I'+str(H/'native'),'-I'+str(H/'third_party/xz'),'-I'+str(B),H/'native/test_resolver.cpp',H/'native/resolver.cpp',H/'native/elf_image.cpp',*objects,'-pthread','-o',binary)
for name,file,index,ready,suffix in required_cases(OUT):
    plan=OUT/(name+'.json');run(binary,index,str(file)+suffix,plan,env=dict(os.environ,HG_EXPECT_READY=str(ready)))
    parsed=json.loads(plan.read_text());assert parsed['ready']==ready,name
    check_plan(name,parsed)
run(sys.executable,H/'adaptive/test_runtime_plans.py',*runtime_plan_arguments(OUT))
report_compatibility(lambda file,suffix:subprocess.run([str(binary),'0',str(file)+suffix],capture_output=True,text=True).stdout)
print('PASS host native resolver, reference ELF/APK, changed layouts, isolated failures and relocated ARM64 execution')
