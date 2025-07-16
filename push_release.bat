@echo off
REM Komut penceresinin Türkçe karakterleri doğru göstermesi için kod sayfasını ayarla
chcp 65001 > nul

:: Script'in bulunduğu dizine geç
cd /D %~dp0

REM --- KULLANICI AYARI ---
set "BUILD_NUM_FILE_PATH=E:\_CODE_\AutoDoorV2\version_build.txt"
REM ---------------------

echo.
echo ===========================================
echo == GateControl Firmware Release Gonderici ==
echo ===========================================
echo.

echo --- 1. Adim: Sunucudaki degisiklikler cekiliyor (pull)...
git pull origin main
echo.

echo --- 2. Adim: Yerel degisiklikler Git'e ekleniyor (add)...
git add .
echo.

echo --- 3. Adim: Degisiklikler paketleniyor (commit)...
REM Build numarasini dosyadan oku
set /p BUILD_NUM=<"%BUILD_NUM_FILE_PATH%"
REM Not: Eger commit edilecek bir sey yoksa, bu komut sadece bir uyari mesaji basar ama script durmaz.
git commit -m "Build %BUILD_NUM%"
echo.

echo --- 4. Adim: Tum yerel commit'ler sunucuya gonderiliyor (push)...
git push origin main
echo.

echo ===========================================
echo == Islem Tamamlandi!                     ==
echo ===========================================
echo.

pause