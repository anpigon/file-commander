#!/bin/sh

set -e

MYSELF="$(basename $0)"

VOL="FileCommander"

APPDIR="FileCommander.app"

QTPATH=$1

rm -rf bin
echo "${MYSELF}: Building the app"
${QTPATH}/bin/qmake -spec macx-clang -r -config release CONFIG+=release "DEFINES+=NDEBUG"
make -j

echo "${MYSELF}: deploying Qt frameworks"
${QTPATH}/bin/macdeployqt bin/release/x64/${APPDIR}

echo "${MYSELF}: creating DMG"

cd bin/release/x64

DMG="${VOL}.dmg"
TMP_DMG="tmp_$$_${DMG}"

# create temporary image
hdiutil create "${TMP_DMG}" -ov -fs "HFS+" -volname "${VOL}" -size 200m
hdiutil attach "${TMP_DMG}"

cp -R ./${APPDIR} /Volumes/${VOL}/
ln -s /Applications /Volumes/${VOL}/

hdiutil detach "/Volumes/${VOL}"
hdiutil resize "${TMP_DMG}" -size min
hdiutil attach "${TMP_DMG}"

echo '
tell application "Finder"
  tell disk "'${VOL}'"
    open
    update without registering applications
    delay 2
    set current view of container window to icon view
    update without registering applications
    delay 2
    set toolbar visible of container window to false
    update without registering applications
    delay 2
    set statusbar visible of container window to false
    update without registering applications
    delay 2
    set the bounds of container window to {400, 100, 899, 356}
    update without registering applications
    delay 2
    set theViewOptions to the icon view options of container window
    update without registering applications
    delay 2
    set arrangement of theViewOptions to not arranged
    update without registering applications
    delay 2
    set icon size of theViewOptions to 72
    update without registering applications
    delay 2
    set position of item "Applications" of container window to {400, 90}
    update without registering applications
    delay 2
    set position of item "'${APPDIR}'" of container window to {100, 90}
    update without registering applications
    delay 2
    eject
  end tell
end tell
' | osascript

#convert to compressed image, delete temp image
rm -f "$DMG"
hdiutil convert "${TMP_DMG}" -format UDZO -o "${DMG}"
rm -f "${TMP_DMG}"
mv "${DMG}" ../../../

echo "${MYSELF}: ready for distribution: ${DMG}"
