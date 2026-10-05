@echo off
if "%1"=="" (echo Aufruf: flash_windows.bat COM5 & exit /b 1)
python -m esptool --chip esp32s3 --port %1 --baud 921600 write-flash 0x0 firmware\ADSB-Map-1.0.0_0x0.bin
