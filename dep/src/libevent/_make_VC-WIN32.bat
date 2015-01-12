IF DEFINED VS120COMNTOOLS (
  SET VCVARSALL="%VS120COMNTOOLS%..\..\VC\vcvarsall.bat"
) ELSE IF DEFINED VS110COMNTOOLS (
  SET VCVARSALL="%VS110COMNTOOLS%..\..\VC\vcvarsall.bat"
) ELSE IF DEFINED VS100COMNTOOLS (
  SET VCVARSALL="%VS100COMNTOOLS%..\..\VC\vcvarsall.bat"
)
IF NOT DEFINED VCVARSALL (
  ECHO Can not find VC2010 or VC2012 or VC2013 installed!
  GOTO ERROR
)
CALL %VCVARSALL% x86
SET rootdir=%~dp0
SET target=WIN32
CD /D "%rootdir%"
nmake /f Makefile.nmake BUILD_TYPE=Debug
RD /S /Q "%rootdir%..\..\include\%target%\libevent"
MD "%rootdir%..\..\include\%target%\libevent"
XCOPY "%rootdir%include\*.h" "%rootdir%..\..\include\%target%\libevent" /S /Y
XCOPY "%rootdir%WIN32-Code\*.h" "%rootdir%..\..\include\%target%\libevent" /S /Y
MD "%rootdir%..\..\lib\%target%"
XCOPY "libevent_*_d.lib" "%rootdir%..\..\lib\%target%" /S /Y
nmake /f Makefile.nmake clean BUILD_TYPE=Debug
nmake /f Makefile.nmake BUILD_TYPE=Release
RD /S /Q "%rootdir%..\..\include\%target%\libevent"
MD "%rootdir%..\..\include\%target%\libevent"
XCOPY "%rootdir%include\*.h" "%rootdir%..\..\include\%target%\libevent" /S /Y
XCOPY "%rootdir%WIN32-Code\*.h" "%rootdir%..\..\include\%target%\libevent" /S /Y
MD "%rootdir%..\..\lib\%target%"
XCOPY "libevent_*.lib" "%rootdir%..\..\lib\%target%" /S /Y
nmake /f Makefile.nmake clean BUILD_TYPE=Release
GOTO EOF

:ERROR
PAUSE

:EOF
