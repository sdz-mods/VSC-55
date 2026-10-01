@ECHO OFF
ECHO   Remove VSC-55 MIDI registration and Windows tray startup.
ECHO   Close games first. VSC-55 will be stopped automatically.
ECHO   Application files, settings and ROMs will remain.
IF "%1"=="/quiet" GOTO begin
PAUSE
:begin
IF NOT EXIST C:\VSC-55\MIDIREG.EXE GOTO missing
C:\VSC-55\MIDIREG.EXE stop
IF ERRORLEVEL 1 GOTO failed
C:\VSC-55\MIDIREG.EXE remove
IF ERRORLEVEL 1 GOTO failed
C:\VSC-55\MIDIREG.EXE startup-off
IF ERRORLEVEL 1 GOTO failed
ECHO   Uninstalled. Reboot to unload the MIDI driver.
ECHO   After reboot, you may delete C:\VSC-55 if its ROMs/settings
ECHO   are no longer needed. The inactive system driver file is retained.
GOTO end
:missing
ECHO   Cannot find C:\VSC-55\MIDIREG.EXE. Restore the installation
ECHO   helper from the package and retry. No registration was changed.
GOTO end
:failed
ECHO   Removal did not finish. Check the messages above and retry.
:end
IF "%1"=="/quiet" GOTO done
PAUSE
:done
