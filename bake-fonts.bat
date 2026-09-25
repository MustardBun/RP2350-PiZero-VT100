@echo off
REM ---------------------------------------------------------------------------
REM  Bake the terminal fonts from the sources listed in tools\fonts.json.
REM
REM  Edit tools\fonts.json to try different TTF or BDF files, then run this
REM  script, then rebuild to produce a new UF2.
REM ---------------------------------------------------------------------------
setlocal

set "HERE=%~dp0"
set "PYTHON="

REM Prefer a python on PATH, else fall back to the py launcher.
where python >nul 2>&1 && set "PYTHON=python"
if not defined PYTHON where py >nul 2>&1 && set "PYTHON=py"
if not defined PYTHON (
    echo ERROR: no python interpreter found. Install Python, or edit this file
    echo        to point PYTHON at your interpreter.
    exit /b 1
)

echo Using %PYTHON%
"%PYTHON%" -c "import PIL" >nul 2>&1
if errorlevel 1 (
    echo Pillow not found - installing...
    "%PYTHON%" -m pip install --quiet pillow
    if errorlevel 1 (
        echo ERROR: could not install Pillow.
        exit /b 1
    )
)

"%PYTHON%" "%HERE%tools\gen_fonts.py"
if errorlevel 1 (
    echo.
    echo Font bake FAILED.
    exit /b 1
)

echo.
echo Done. Rebuild to pick up the new font tables.
exit /b 0
