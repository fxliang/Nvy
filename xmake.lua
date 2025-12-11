add_rules("mode.release", "mode.debug")
set_languages("c++20")
set_runtimes(is_mode("release") and "MT" or "MTd")

target("Nvy")
  set_kind("binary")
  add_files("resources/third_party/nvim_icon.rc", "version_info.rc")
  add_headerfiles(
    "src/common/dx_helper.h",
    "src/common/mpack_helper.h",
    "src/common/vec.h",
    "src/common/window_messages.h",
    "src/nvim/nvim.h",
    "src/renderer/glyph_renderer.h",
    "src/renderer/renderer.h",
    "src/third_party/mpack/mpack.h",
    "src/pch.h"
  )
  add_files(
    "src/main.cpp",
    "src/nvim/nvim.cpp",
    "src/renderer/glyph_renderer.cpp",
    "src/renderer/renderer.cpp",
    "src/third_party/mpack/mpack.c"
  )
  add_includedirs("src")
  add_links("user32", "shell32", "advapi32", "d3d11", "d2d1", "dwrite", "Shcore", "Dwmapi", "imm32")
  add_defines("MPACK_EXTENSIONS", "UNICODE")
  -- Check if the compiler is MSVC
  if is_plat("windows") and toolchain("msvc") then
    -- Replace /GR with /GR- and /EHsc with /EHs-c- for MSVC
    add_cxxflags("/GR-")  -- Disable RTTI
    add_cxxflags("/EHs-c-")  -- Disable exceptions
  else
    -- For non-MSVC compilers, disable RTTI and exceptions
    add_cxxflags("-fno-rtti")
    add_cxxflags("-fno-exceptions")
  end

  on_load(function (target)
    import("core.project.config")
    import("core.base.semver")
    import("lib.detect.find_tool")

    local version_major, version_minor, version_patch, extra_commits, commit_hash = "0", "0", "0", "0", "-local"
    -- Check for Git executable
    if find_tool("git") then
      try {
        -- Get the version from Git using describe
        function ()
          local git_version = os.iorun("git describe --tags --dirty --match \"v*\"")
          -- Match the version string using a regex pattern
          local major, minor, patch, extra, commit = git_version:match("v([0-9]+)%.([0-9]+)%.([0-9]+)%-([0-9]+)%-(g[0-9a-f]+%-?.*)")
          if major then
            version_major = major
            version_minor = minor
            version_patch = patch
            extra_commits = extra or "0"
            commit_hash = commit or "-local"
          else
            print("Warning: Bad version, falling back to 0")
          end
        end
      } catch {}
    end
    local version_info_file = "version_info.rc"
    -- Process version info using template
    if os.exists("resources/version_info.rc.in") then
      local rc_content = io.open("resources/version_info.rc.in", "r"):read("*all")

      local product_version = ""..version_major..","..version_minor..","..version_patch..",".. extra_commits
      local file_version = ""..version_major..","..version_minor..","..version_patch..","..extra_commits

      local file_version_str = "\"" .. file_version:gsub(",", ".") .. "\""
      local product_version_str = ('"'..version_major.."."..version_minor.."."..version_patch..".")..commit_hash .. '"'

      rc_content = rc_content:gsub('"FileVersion",      VER_FILE_VERSION_STR', '"FileVersion",      ' .. file_version_str)
      rc_content = rc_content:gsub('"ProductVersion",   VER_PRODUCT_VERSION_STR', '"ProductVersion",   ' .. product_version_str)

      rc_content = rc_content:gsub("FILEVERSION        VER_FILE_VERSION", "FILEVERSION        " .. file_version)
      rc_content = rc_content:gsub("PRODUCTVERSION     VER_PRODUCT_VERSION", "PRODUCTVERSION     " .. product_version)
      -- Write the processed content to version_info.rc
      local file = io.open(version_info_file, "w")
      file:write(rc_content)
      file:close()
    end
  end)
