@echo off
pip show requests >nul 2>&1 || pip install requests
python "%~dp0eye_manager.py"
