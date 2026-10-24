@echo off
rem STM32 IoT: InfluxDB ve Node-RED'i ayri pencerelerde baslatir.
rem Mosquitto ve Grafana Windows servisi olarak zaten kendiliginden calisir.
start "InfluxDB" "%LOCALAPPDATA%\Microsoft\WinGet\Packages\InfluxData.InfluxDB.OSS_Microsoft.Winget.Source_8wekyb3d8bbwe\influxd.exe" --reporting-disabled
timeout /t 3 /nobreak >nul
start "Node-RED" cmd /k node-red
timeout /t 8 /nobreak >nul
start "" http://localhost:3000
start "" http://localhost:1880/dashboard/izleme
