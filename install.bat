@echo off

IF "%QHOME%"=="" (
    ECHO ERROR: Enviroment variable QHOME is NOT defined 
    EXIT /B
)

IF NOT EXIST %QHOME%\w64 (
    ECHO ERROR: Installation destination %QHOME%\w64 does not exist
    EXIT /B
)

IF NOT EXIST lib (
    ECHO ERROR: Directory 'lib' does not exist. Please run from release package
    EXIT /B
)

IF NOT EXIST q (
    ECHO ERROR: Directory 'q' does not exist. Please run from release package
    EXIT /B
)

ECHO Copying q script to %QHOME%
COPY q\* %QHOME%
IF %ERRORLEVEL% NEQ 0 (
    ECHO ERROR: Copy failed
    EXIT /B %ERRORLEVEL%
)

ECHO Copying DLL to %QHOME%\w64
COPY lib\* %QHOME%\w64\
IF %ERRORLEVEL% NEQ 0 (
    ECHO ERROR: Copy failed
    EXIT /B %ERRORLEVEL%
)

ECHO Installation complete