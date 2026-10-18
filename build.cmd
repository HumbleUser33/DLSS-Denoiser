@ECHO OFF
REM Builds the shaders, the bench and the VapourSynth plugin into _build\.
REM MSVC 14.44, the v143 tools, as MPCVR-Plus releases are built: under Visual Studio
REM 2026 they are taken by their version when installed.
REM
REM   build.cmd          everything
REM   build.cmd shaders  the shaders and their resource only
REM   build.cmd plugin   the shaders and the plugin, not the bench

SETLOCAL
PUSHD %~dp0

FOR /F "usebackq tokens=*" %%i IN (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) DO SET VSPATH=%%i
IF "%VSPATH%"=="" (
  ECHO Visual Studio not found.
  POPD
  EXIT /B 1
)
SET VCVER=
SET "V143=%VSPATH%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt"
IF EXIST "%V143%" FOR /F "usebackq tokens=*" %%v IN ("%V143%") DO SET VCVER=-vcvars_ver=%%v
CALL "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo %VCVER% || (POPD & EXIT /B 1)
ECHO MSVC %VCToolsVersion%

IF NOT EXIST _build MKDIR _build

ECHO Compiling the shaders...
SET SH=src\shaders
SET FXC=fxc /nologo /O3 /T cs_5_0
%FXC% /Fo _build\cs_motion_frame.cso  %SH%\cs_motion.hlsl /DPASS=0 >NUL || GOTO :fail
%FXC% /Fo _build\cs_motion_block.cso  %SH%\cs_motion.hlsl /DPASS=4 >NUL || GOTO :fail
%FXC% /Fo _build\cs_motion_expand.cso %SH%\cs_motion.hlsl /DPASS=3 >NUL || GOTO :fail
%FXC% /Fo _build\cs_global_motion.cso %SH%\cs_dlss_global_motion.hlsl >NUL || GOTO :fail
%FXC% /Fo _build\cs_global_step.cso   %SH%\cs_dlss_global_refine.hlsl /DPASS=1 >NUL || GOTO :fail
%FXC% /Fo _build\cs_global_solve.cso  %SH%\cs_dlss_global_refine.hlsl /DPASS=2 >NUL || GOTO :fail
%FXC% /Fo _build\cs_scene_sums.cso    %SH%\cs_dlss_scene_cut.hlsl /DPASS=1 >NUL || GOTO :fail
%FXC% /Fo _build\cs_scene_cut.cso     %SH%\cs_dlss_scene_cut.hlsl /DPASS=2 >NUL || GOTO :fail
%FXC% /Fo _build\cs_convert_in.cso    %SH%\cs_convert.hlsl /DPASS=0 >NUL || GOTO :fail
%FXC% /Fo _build\cs_convert_out.cso   %SH%\cs_convert.hlsl /DPASS=1 >NUL || GOTO :fail
%FXC% /Fo _build\cs_tiles.cso          %SH%\cs_tiles.hlsl >NUL || GOTO :fail
%FXC% /Fo _build\cs_blur_h.cso         %SH%\cs_blur.hlsl /DPASS=0 >NUL || GOTO :fail
%FXC% /Fo _build\cs_blur_v.cso         %SH%\cs_blur.hlsl /DPASS=1 >NUL || GOTO :fail
%FXC% /Fo _build\cs_blur_grad.cso      %SH%\cs_blur.hlsl /DPASS=2 >NUL || GOTO :fail
%FXC% /Fo _build\cs_tile_median.cso    %SH%\cs_tiles.hlsl /DMEDIAN >NUL || GOTO :fail
rc /nologo /Isrc /fo _build\shaders.res src\shaders.rc || GOTO :fail
IF /I "%1"=="shaders" GOTO :done

SET CFLAGS=/nologo /EHsc /std:c++20 /O2 /MT /W4 /wd4100 /wd4505 /openmp /DUNICODE /D_UNICODE /Fo_build\ /Fd_build\
SET ENGINE=src\DlssSR.cpp src\D3D11Util.cpp src\DlssOpticalFlow.cpp src\MotionField.cpp

IF /I "%1"=="plugin" GOTO :plugin
ECHO Building the bench...
cl %CFLAGS% bench\bench.cpp %ENGINE% src\Engine.cpp _build\shaders.res /Fe:_build\dlssdn_bench.exe || GOTO :fail

:plugin
REM VapourSynth's API 4.2 header, the one its R79 runtime matches: StaxRip's copy unless VSINC says
REM otherwise. The header in VapourSynth\include beside it declares API 4.3, which R79 lacks.
IF "%VSINC%"=="" SET "VSINC=E:\StaxRip-v2.52.6-x64\Apps\FrameServer\VapourSynth\Lib\site-packages\vapoursynth\include"
ECHO Building the plugin...
cl %CFLAGS% /LD /I"%VSINC%" src\plugin.cpp %ENGINE% src\Engine.cpp _build\shaders.res /Fe:_build\vsdlss.dll /link /DLL || GOTO :fail

:done
POPD
EXIT /B 0

:fail
POPD
EXIT /B 1
