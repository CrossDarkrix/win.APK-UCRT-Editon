./windows/scripts/build-windows-ucrt64-gstreamer-fixed.sh
python3 second_setting.py
./windows/scripts/stage-ucrt-runtime.sh
cp -a  build-windows-ucrt64/Win.apk.exe Win.apk-UCRT/Win.apk.exe
cp -a build-windows-ucrt64/winapk-runtime-dist/* Win.apk-UCRT/
rm -rf Win.apk-UCRT/clang
rm -rf Win.apk-UCRT/.build
rm Win.apk-UCRT/bin/Win.apk.exe
rm Win.apk-UCRT/run-winapk-launcher.cmd
rm Win.apk-UCRT/run-winapk-runtime.cmd
python jdk_runtime_preflight.py --project-root "." --jdk "C:/Program Files/Zulu/zulu-21" --runtime-dist "Win.apk-UCRT" --output-dir "build/phase20-a1" --require-artifacts
python jdk_module_analysis.py --project-root "." --runtime-dist "Win.apk-UCRT" --jdk "C:/Program Files/Zulu/zulu-21" --output-dir "build/phase20-a2"
python jdk_runtime_image.py --project-root "." --a2-dir "build/phase20-a2" --jdk "C:/Program Files/Zulu/zulu-21" --output-dir "build/phase20-a3"
python runtime_stage_integration.py --project-root "." --a3-dir "build/phase20-a3" --stage-dir "Win.apk-UCRT" --output-dir "build/phase20-a4"
python NSICreator.py
echo "Done!"