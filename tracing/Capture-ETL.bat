@echo off
pushd %~dp0
wpr -start DriverTracing.wprp -filemode

echo Reproduce your issue now, then
pause

wpr -stop DriverTracing.etl
popd
