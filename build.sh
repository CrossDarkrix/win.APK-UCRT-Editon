./windows/scripts/build-windows-ucrt64-gstreamer-fixed.sh
python3 second_setting.py
./windows/scripts/stage-ucrt-runtime.sh
cp -a  build-windows-ucrt64/Win.apk.exe Win.apk-UCRT/Win.apk.exe
cp -a build-windows-ucrt64/winapk-runtime-dist/* Win.apk-UCRT/
rm -rf Win.apk-UCRT/clang
rm -rf Win.apk-UCRT/.build
rm Win.apk-UCRT/run-winapk-runtime.cmd
echo "Done!"