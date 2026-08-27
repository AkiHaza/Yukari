@echo off
where gradle >nul 2>nul
if errorlevel 1 (
  echo gradlew: Gradle was not found on PATH.
  echo Install Gradle 8.11.1 or run this project in CI with setup-gradle.
  exit /b 127
)
gradle %*
