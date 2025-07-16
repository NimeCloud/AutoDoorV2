@echo off
setlocal enabledelayedexpansion

:: Kod sayfasını UTF-8 yap
chcp 65001 > nul

:: Log dosyası
set LOGFILE=release.log
echo --------------------------------------------- > %LOGFILE%
echo [Başlangıç: %DATE% %TIME%] >> %LOGFILE%
echo --------------------------------------------- >> %LOGFILE%

echo ============================================ 
echo == GateControl Firmware Release Aracı    ==
echo ============================================ 

echo ============================================ >> %LOGFILE%
echo == GateControl Firmware Release Aracı    == >> %LOGFILE%
echo ============================================ >> %LOGFILE%

:: --- KULLANICI AYARI -------------------------
set "BUILD_NUM_FILE_PATH=E:\_CODE_\AutoDoorV2\version_build.txt"
:: ---------------------------------------------

if not exist "%BUILD_NUM_FILE_PATH%" (
    echo [HATA] Build numarası dosyası bulunamadı: %BUILD_NUM_FILE_PATH%
    echo [HATA] Build numarası dosyası bulunamadı: %BUILD_NUM_FILE_PATH% >> %LOGFILE%
    pause
    exit /b
)

set /p BUILD_NUM=<"%BUILD_NUM_FILE_PATH%"
echo - Bulunan Build numarası: %BUILD_NUM%
echo - Bulunan Build numarası: %BUILD_NUM% >> %LOGFILE%

cd release

echo git add . >> %LOGFILE%
git add . >> %LOGFILE% 2>&1
if %errorlevel% neq 0 (
    echo [HATA] git add sırasında sorun oluştu.
    echo [HATA] git add sırasında sorun oluştu. >> %LOGFILE%
    pause
    exit /b
)
echo - Tüm değişiklikler eklendi.
echo - Tüm değişiklikler eklendi. >> %LOGFILE%

echo git commit -m "Release Build %BUILD_NUM% surumu yayinlandi." >> %LOGFILE%
git commit -m "Release Build %BUILD_NUM% surumu yayinlandi." >> %LOGFILE% 2>&1
if %errorlevel% neq 0 (
    echo [Uyarı] Commit atılacak değişiklik bulunamadı.
    echo [Uyarı] Commit atılacak değişiklik bulunamadı. >> %LOGFILE%
) else (
    echo - Değişiklikler commit edildi.
    echo - Değişiklikler commit edildi. >> %LOGFILE%
)

echo git pull origin main >> %LOGFILE%
git pull origin main >> %LOGFILE% 2>&1
if %errorlevel% neq 0 (
    echo [HATA] git pull başarısız.
    echo [HATA] git pull başarısız. >> %LOGFILE%
    pause
    exit /b
)
echo - Sunucudan en güncel hali çekildi.
echo - Sunucudan en güncel hali çekildi. >> %LOGFILE%

echo git push origin main >> %LOGFILE%
git push origin main >> %LOGFILE% 2>&1
if %errorlevel% neq 0 (
    echo [HATA] git push başarısız.
    echo [HATA] git push başarısız. >> %LOGFILE%
    pause
    exit /b
)
echo - Değişiklikler başarıyla GitHub'a gönderildi.
echo - Değişiklikler başarıyla GitHub'a gönderildi. >> %LOGFILE%

git status >> %LOGFILE% 2>&1
git log -1 >> %LOGFILE% 2>&1

echo [✓] İşlem tamamlandı!
echo [✓] İşlem tamamlandı! >> %LOGFILE%
echo --------------------------------------------- >> %LOGFILE%
echo [Bitiş: %DATE% %TIME%] >> %LOGFILE%

start notepad %LOGFILE%
pause
