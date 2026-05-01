# Setup Instructions for skyrim_render_clean

## Required: SKSE 1.7.3 Source Code

Before building, you must download and set up the SKSE 1.7.3 source code:

1. **Download SKSE 1.7.3 source**: 
   - Get it from https://github.com/ianpatt/skse64/tree/1.5.97
   - Or extract from the official SKSE 1.7.3 installer

2. **Set SKSE_ROOT environment variable**:
   - Open Windows Settings > System > About > Advanced system settings
   - Click "Environment Variables..."
   - Under "User variables", click "New..."
   - Variable name: `SKSE_ROOT`
   - Variable value: Full path to your SKSE source directory (the one containing `common/` and `skse/` folders)
   - Example: `C:\dev\skse\src`

3. **Verify SKSE_ROOT setup**:
   - Open a new Command Prompt
   - Run: `echo %SKSE_ROOT%`
   - It should show your SKSE source path
   - Verify the folder contains: `common\ITypes.h` and `skse\skse\PluginAPI.h`

## Alternative: Manual Path Configuration

If you prefer not to use environment variables, you can edit the project file:

1. Open `skyrim_render_clean.vcxproj` in a text editor
2. Find this line:
   ```xml
   <AdditionalIncludeDirectories>$(ProjectDir)include;$(ProjectDir)include\core;$(ProjectDir)include\hooks;$(ProjectDir)include\optimize;$(ProjectDir)include\profile;$(ProjectDir)include\arch;$(SKSE_ROOT);$(SKSE_ROOT)\common;$(SKSE_ROOT)\skse;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
   ```
3. Replace `$(SKSE_ROOT);$(SKSE_ROOT)\common;$(SKSE_ROOT)\skse` with your actual SKSE paths:
   ```xml
   <AdditionalIncludeDirectories>$(ProjectDir)include;$(ProjectDir)include\core;$(ProjectDir)include\hooks;$(ProjectDir)include\optimize;$(ProjectDir)include\profile;$(ProjectDir)include\arch;C:\dev\skse\src;C:\dev\skse\src\common;C:\dev\skse\src\skse;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
   ```

## Building

Once SKSE_ROOT is set:

1. Open `skyrim_render_clean.sln` in Visual Studio 2022/2019
2. Build > Build Solution (or press F7)
3. The DLL will be created in `Release\skyrim_render_clean.dll`

## Troubleshooting

**Error: Cannot open include file: 'common/ITypes.h'**
- SKSE_ROOT is not set or pointing to wrong directory
- Verify the path contains the `common` folder with `ITypes.h`

**Error: MSVCP140.dll not found**
- Make sure you're building Release configuration
- Verify Runtime Library is set to Multi-threaded (/MT)

**Other build errors**
- Ensure you have Visual Studio 2022 or 2019 with C++ development tools
- Make sure Windows SDK is installed
