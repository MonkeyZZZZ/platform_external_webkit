mkdir 2>NUL "%CONFIGURATIONBUILDDIR%\bin\WebKit.resources\inspector"
xcopy /y /d /s /exclude:xcopy.excludes "%ProjectDir%..\inspector\front-end\*" "%CONFIGURATIONBUILDDIR%\bin\WebKit.resources\inspector"
xcopy /y /d /s /exclude:xcopy.excludes "%CONFIGURATIONBUILDDIR%\obj\WebCore\DerivedSources\InspectorBackendStub.js" "%CONFIGURATIONBUILDDIR%\bin\WebKit.resources\inspector"
mkdir 2>NUL "%CONFIGURATIONBUILDDIR%\bin\WebKit.resources\en.lproj"
xcopy /y /d /s /exclude:xcopy.excludes "%ProjectDir%..\English.lproj\localizedStrings.js" "%CONFIGURATIONBUILDDIR%\bin\WebKit.resources\en.lproj"