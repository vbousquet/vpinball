

.\shaderc.exe -f vs_debug.sc -o metal/vs_debug.bin --platform windows -p metal --type vertex
.\shaderc.exe -f fs_debug.sc -o metal/fs_debug.bin --platform windows -p metal --type fragment
.\shaderc.exe -f vs_debug.sc -o gles/vs_debug.bin --platform windows -p 320_es --type vertex
.\shaderc.exe -f fs_debug.sc -o gles/fs_debug.bin --platform windows -p 320_es --type fragment
.\shaderc.exe -f vs_debug.sc -o glsl/vs_debug.bin --platform windows -p 440 --type vertex
.\shaderc.exe -f fs_debug.sc -o glsl/fs_debug.bin --platform windows -p 440 --type fragment
.\shaderc.exe -f vs_debug.sc -o dx9/vs_debug.bin --platform windows -p vs_3_0 -O 3 --type vertex
.\shaderc.exe -f fs_debug.sc -o dx9/fs_debug.bin --platform windows -p ps_3_0 -O 3 --type fragment
.\shaderc.exe -f vs_debug.sc -o dx11/vs_debug.bin --platform windows -p vs_5_0 -O 3 --type vertex
.\shaderc.exe -f fs_debug.sc -o dx11/fs_debug.bin --platform windows -p ps_5_0 -O 3 --type fragment


