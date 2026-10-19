"""Build only externally supplied DXMT airconv as an LGPL shared library."""
from pathlib import Path
import json,shlex,argparse,os
root=Path(__file__).resolve().parent
src=Path(os.environ.get('DXMT_SRC',str(root.parent/'dxmt_src')))   # tools/macos/setup_deps.sh clones DXMT here
entries=json.loads((src/'build/compile_commands.json').read_text())
selected=[e for e in entries if 'darwin/libairconv.a.p/' in e['output'] or 'libDXBCParserNative.a.p/' in e['output']]
files=[];includes=[]
for e in selected:
 files.append(str((src/'build'/e['file']).resolve()))
 for t in shlex.split(e['command']):
  if t.startswith('-I'):
   d=str((src/'build'/t[2:]).resolve())
   if Path(d).exists() and d not in includes: includes.append(d)
parser=argparse.ArgumentParser()
parser.add_argument('--llvm',type=Path,default=src.parent/'llvm15_x86_64',help='LLVM 15 installation with arm64 libraries')
llvm=parser.parse_args().llvm.resolve()
includes=[d for d in includes if 'llvm15_x86_64/include' not in d]
text='cmake_minimum_required(VERSION 3.20)\nproject(airconv_native LANGUAGES C CXX)\nset(CMAKE_CXX_STANDARD 20)\n'
text+='find_package(LLVM 15 REQUIRED CONFIG PATHS "'+str(llvm/'lib/cmake/llvm')+'" NO_DEFAULT_PATH)\n'
text+='llvm_map_components_to_libnames(AIRCONV_LLVM_LIBS bitwriter passes)\n'
text+='add_library(airconv SHARED\n'+''.join(' "'+f+'"\n' for f in files)+')\n'
text+='target_include_directories(airconv PRIVATE ${LLVM_INCLUDE_DIRS}\n'+''.join(' "'+d+'"\n' for d in includes)+')\n'
text+='target_compile_options(airconv PRIVATE -fblocks -fno-rtti -Wno-extern-c-compat -Wno-microsoft-exception-spec)\n'
text+='target_compile_definitions(airconv PRIVATE NOMINMAX _WIN32_WINNT=0xa00 DXMT_PAGE_SIZE=4096)\n'
text+='target_link_libraries(airconv PRIVATE ${AIRCONV_LLVM_LIBS})\n'
(root/'CMakeLists.txt').write_text(text)
print(len(files),'external sources; LLVM:',llvm)
