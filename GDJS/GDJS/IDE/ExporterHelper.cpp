/*
 * GDevelop JS Platform
 * Copyright 2008-2016 Florian Rival (Florian.Rival@gmail.com). All rights
 * reserved. This project is released under the MIT License.
 */
#include "GDJS/IDE/ExporterHelper.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <streambuf>
#include <string>

#include "GDCore/CommonTools.h"
#include "GDCore/Events/CodeGeneration/EffectsCodeGenerator.h"
#include "GDCore/IDE/AbstractFileSystem.h"
#include "GDCore/IDE/Project/ProjectResourcesCopier.h"
#include "GDCore/IDE/ProjectStripper.h"
#include "GDCore/IDE/SceneNameMangler.h"
#include "GDCore/Project/ExternalEvents.h"
#include "GDCore/Project/ExternalLayout.h"
#include "GDCore/Project/Layout.h"
#include "GDCore/Project/Project.h"
#include "GDCore/Project/SourceFile.h"
#include "GDCore/Serialization/Serializer.h"
#include "GDCore/TinyXml/tinyxml.h"
#include "GDCore/Tools/Localization.h"
#include "GDCore/Tools/Log.h"
#include "GDJS/Events/CodeGeneration/LayoutCodeGenerator.h"
#undef CopyFile  // Disable an annoying macro

namespace gdjs {

static void InsertUnique(std::vector<gd::String> &container, gd::String str) {
  if (std::find(container.begin(), container.end(), str) == container.end())
    container.push_back(str);
}

ExporterHelper::ExporterHelper(gd::AbstractFileSystem &fileSystem,
                               gd::String gdjsRoot_,
                               gd::String codeOutputDir_)
    : fs(fileSystem), gdjsRoot(gdjsRoot_), codeOutputDir(codeOutputDir_){};

bool ExporterHelper::ExportProjectForPixiPreview(
    const PreviewExportOptions &options) {
  fs.MkDir(options.exportPath);
  fs.ClearDir(options.exportPath);
  std::vector<gd::String> includesFiles;

  gd::Project exportedProject = options.project;

  // Always disable the splash for preview
  exportedProject.GetLoadingScreen().ShowGDevelopSplash(false);

  // Export resources (*before* generating events as some resources filenames
  // may be updated)
  ExportResources(fs, exportedProject, options.exportPath);

  // Compatibility with GD <= 5.0-beta56
  // Stay compatible with text objects declaring their font as just a filename
  // without a font resource - by manually adding these resources.
  AddDeprecatedFontFilesToFontResources(
      fs, exportedProject.GetResourcesManager(), exportDir);
  // end of compatibility code

  // Export engine libraries
  AddLibsInclude(true, false, true, includesFiles);

  // Export effects (after engine libraries as they auto-register themselves to
  // the engine)
  ExportEffectIncludes(exportedProject, includesFiles);

  // Generate events code
  if (!ExportEventsCode(exportedProject, codeOutputDir, includesFiles, true))
    return false;

  // Export source files
  if (!ExportExternalSourceFiles(
          exportedProject, codeOutputDir, includesFiles)) {
    gd::LogError(_("Error during exporting! Unable to export source files:\n") +
                 lastError);
    return false;
  }

  // Strip the project (*after* generating events as the events may use stripped
  // things (objects groups...))
  gd::ProjectStripper::StripProjectForExport(exportedProject);
  exportedProject.SetFirstLayout(options.layoutName);

  // Strip the includes to only have Pixi.js files (*before* creating runtimeGameOptions,
  // since otherwise Cocos files will be passed to the hot-reloader).
  RemoveIncludes(false, true, includesFiles);

  // Create the setup options passed to the gdjs.RuntimeGame
  gd::SerializerElement runtimeGameOptions;
  runtimeGameOptions.AddChild("isPreview").SetBoolValue(true);
  if (!options.externalLayoutName.empty()) {
    runtimeGameOptions.AddChild("injectExternalLayout")
        .SetValue(options.externalLayoutName);
  }

  // Pass in the options the list of scripts files - useful for hot-reloading.
  auto &scriptFilesElement = runtimeGameOptions.AddChild("scriptFiles");
  scriptFilesElement.ConsiderAsArrayOf("scriptFile");
  for (const auto &includeFile : includesFiles) {
    auto hashIt = options.includeFileHashes.find(includeFile);
    scriptFilesElement.AddChild("scriptFile")
        .SetStringAttribute("path", includeFile)
        .SetIntAttribute(
            "hash",
            hashIt != options.includeFileHashes.end() ? hashIt->second : 0);
  }

  // Export the project
  ExportProjectData(
      fs, exportedProject, codeOutputDir + "/data.js", runtimeGameOptions);
  includesFiles.push_back(codeOutputDir + "/data.js");

  // Copy all the dependencies
  ExportIncludesAndLibs(includesFiles, options.exportPath, false);

  // Create the index file
  if (!ExportPixiIndexFile(exportedProject,
                           gdjsRoot + "/Runtime/index.html",
                           options.exportPath,
                           includesFiles,
                           "gdjs.runtimeGameOptions"))
    return false;

  return true;
}

gd::String ExporterHelper::ExportProjectData(
    gd::AbstractFileSystem &fs,
    const gd::Project &project,
    gd::String filename,
    const gd::SerializerElement &runtimeGameOptions) {
  fs.MkDir(fs.DirNameFrom(filename));

  // Save the project to JSON
  gd::SerializerElement rootElement;
  project.SerializeTo(rootElement);
  gd::String output =
      "gdjs.projectData = " + gd::Serializer::ToJSON(rootElement) + ";\n" +
      "gdjs.runtimeGameOptions = " +
      gd::Serializer::ToJSON(runtimeGameOptions) + ";\n";

  if (!fs.WriteToFile(filename, output)) return "Unable to write " + filename;

  return "";
}

bool ExporterHelper::ExportPixiIndexFile(
    const gd::Project &project,
    gd::String source,
    gd::String exportDir,
    const std::vector<gd::String> &includesFiles,
    gd::String additionalSpec) {
  gd::String str = fs.ReadFile(source);

  // Generate the file
  if (!CompleteIndexFile(str, exportDir, includesFiles, additionalSpec))
    return false;

  // Write the index.html file
  if (!fs.WriteToFile(exportDir + "/index.html", str)) {
    lastError = "Unable to write index file.";
    return false;
  }

  return true;
}

bool ExporterHelper::ExportCordovaFiles(const gd::Project &project,
                                        gd::String exportDir) {
  auto &platformSpecificAssets = project.GetPlatformSpecificAssets();
  auto &resourceManager = project.GetResourcesManager();
  auto getIconFilename = [&resourceManager, &platformSpecificAssets](
                             const gd::String &platform,
                             const gd::String &name) {
    const gd::String &file =
        resourceManager.GetResource(platformSpecificAssets.Get(platform, name))
            .GetFile();
    return file.empty() ? "" : "www/" + file;
  };

  auto makeIconsAndroid = [&getIconFilename]() {
    std::vector<std::pair<gd::String, gd::String>> sizes = {{"36", "ldpi"},
                                                            {"48", "mdpi"},
                                                            {"72", "hdpi"},
                                                            {"96", "xhdpi"},
                                                            {"144", "xxhdpi"},
                                                            {"192", "xxxhdpi"}};

    gd::String output;
    for (auto &size : sizes) {
      gd::String filename = getIconFilename("android", "icon-" + size.first);
      output += !filename.empty() ? ("<icon src=\"" + filename +
                                     "\" density=\"" + size.second + "\" />\n")
                                  : "";
    }

    return output;
  };

  auto makeIconsIos = [&getIconFilename]() {
    std::vector<gd::String> sizes = {"180",
                                     "60",
                                     "120",
                                     "76",
                                     "152",
                                     "40",
                                     "80",
                                     "57",
                                     "114",
                                     "72",
                                     "144",
                                     "167",
                                     "29",
                                     "58",
                                     "50",
                                     "100"};

    gd::String output;
    for (auto &size : sizes) {
      gd::String filename = getIconFilename("ios", "icon-" + size);
      output += !filename.empty() ? ("<icon src=\"" + filename + "\" width=\"" +
                                     size + "\" height=\"" + size + "\" />\n")
                                  : "";
    }

    return output;
  };

  gd::String str =
      fs.ReadFile(gdjsRoot + "/Runtime/Cordova/config.xml")
          .FindAndReplace("GDJS_PROJECTNAME",
                          gd::Serializer::ToEscapedXMLString(project.GetName()))
          .FindAndReplace(
              "GDJS_PACKAGENAME",
              gd::Serializer::ToEscapedXMLString(project.GetPackageName()))
          .FindAndReplace("GDJS_ORIENTATION", project.GetOrientation())
          .FindAndReplace("GDJS_PROJECTVERSION", project.GetVersion())
          .FindAndReplace("<!-- GDJS_ICONS_ANDROID -->", makeIconsAndroid())
          .FindAndReplace("<!-- GDJS_ICONS_IOS -->", makeIconsIos());

  if (!project.GetAdMobAppId().empty()) {
    str = str.FindAndReplace(
        "<!-- GDJS_ADMOB_PLUGIN_AND_APPLICATION_ID -->",
        "<plugin name=\"cordova-plugin-admob-free\" spec=\"~0.21.0\">\n"
        "\t\t<variable name=\"ADMOB_APP_ID\" value=\"" +
            project.GetAdMobAppId() +
            "\" />\n"
            "\t</plugin>");
  }

  if (!fs.WriteToFile(exportDir + "/config.xml", str)) {
    lastError = "Unable to write Cordova config.xml file.";
    return false;
  }

  gd::String jsonName =
      gd::Serializer::ToJSON(gd::SerializerElement(project.GetName()));
  gd::String jsonAuthor =
      gd::Serializer::ToJSON(gd::SerializerElement(project.GetAuthor()));
  gd::String jsonVersion =
      gd::Serializer::ToJSON(gd::SerializerElement(project.GetVersion()));
  gd::String jsonMangledName = gd::Serializer::ToJSON(
      gd::SerializerElement(gd::SceneNameMangler::Get()
                                ->GetMangledSceneName(project.GetName())
                                .LowerCase()
                                .FindAndReplace(" ", "-")));

  {
    gd::String str =
        fs.ReadFile(gdjsRoot + "/Runtime/Cordova/package.json")
            .FindAndReplace("\"GDJS_GAME_NAME\"", jsonName)
            .FindAndReplace("\"GDJS_GAME_AUTHOR\"", jsonAuthor)
            .FindAndReplace("\"GDJS_GAME_VERSION\"", jsonVersion)
            .FindAndReplace("\"GDJS_GAME_MANGLED_NAME\"", jsonMangledName);

    if (!fs.WriteToFile(exportDir + "/package.json", str)) {
      lastError = "Unable to write Cordova package.json file.";
      return false;
    }
  }

  return true;
}

bool ExporterHelper::ExportCocos2dFiles(
    const gd::Project &project,
    gd::String exportDir,
    bool debugMode,
    const std::vector<gd::String> &includesFiles) {
  if (!fs.CopyFile(gdjsRoot + "/Runtime/Cocos2d/main.js",
                   exportDir + "/main.js")) {
    lastError = "Unable to write Cocos2d main.js file.";
    return false;
  }

  if (!fs.CopyFile(gdjsRoot + "/Runtime/Cocos2d/cocos2d-js-v3.10.js",
                   exportDir + "/cocos2d-js-v3.10.js")) {
    lastError = "Unable to write Cocos2d cocos2d-js-v3.10.js file.";
    return false;
  }

  {
    gd::String str = fs.ReadFile(gdjsRoot + "/Runtime/Cocos2d/index.html");

    // Generate custom declarations for font resources
    gd::String customCss;
    gd::String customHtml;

    // Generate the file
    std::vector<gd::String> noIncludesInThisFile;
    if (!CompleteIndexFile(str, exportDir, noIncludesInThisFile, "")) {
      lastError = "Unable to complete Cocos2d-JS index.html file.";
      return false;
    }

    // Write the index.html file
    if (!fs.WriteToFile(exportDir + "/index.html", str)) {
      lastError = "Unable to write Cocos2d-JS index.html file.";
      return false;
    }
  }

  {
    gd::String includeFilesStr = "";
    bool first = true;
    for (auto &file : includesFiles) {
      if (!fs.FileExists(exportDir + "/src/" + file)) {
        std::cout << "Warning: Unable to find " << exportDir + "/" + file << "."
                  << std::endl;
        continue;
      }

      includeFilesStr +=
          gd::String(first ? "" : ", ") + "\"src/" + file + "\"\n";
      first = false;
    }

    gd::String str =
        fs.ReadFile(gdjsRoot + "/Runtime/Cocos2d/project.json")
            .FindAndReplace("// GDJS_INCLUDE_FILES", includeFilesStr)
            .FindAndReplace("/*GDJS_SHOW_FPS*/", debugMode ? "true" : "false");

    if (!fs.WriteToFile(exportDir + "/project.json", str)) {
      lastError = "Unable to write Cocos2d-JS project.json file.";
      return false;
    }
  }

  return true;
}

bool ExporterHelper::ExportFacebookInstantGamesFiles(const gd::Project &project,
                                                     gd::String exportDir) {
  {
    gd::String str =
        fs.ReadFile(gdjsRoot +
                    "/Runtime/FacebookInstantGames/fbapp-config.json")
            .FindAndReplace("\"GDJS_ORIENTATION\"",
                            project.GetOrientation() == "portrait"
                                ? "\"PORTRAIT\""
                                : "\"LANDSCAPE\"");

    if (!fs.WriteToFile(exportDir + "/fbapp-config.json", str)) {
      lastError =
          "Unable to write Facebook Instant Games fbapp-config.json file.";
      return false;
    }
  }

  return true;
}

bool ExporterHelper::ExportElectronFiles(const gd::Project &project,
                                         gd::String exportDir) {
  gd::String jsonName =
      gd::Serializer::ToJSON(gd::SerializerElement(project.GetName()));
  gd::String jsonAuthor =
      gd::Serializer::ToJSON(gd::SerializerElement(project.GetAuthor()));
  gd::String jsonVersion =
      gd::Serializer::ToJSON(gd::SerializerElement(project.GetVersion()));
  gd::String jsonMangledName = gd::Serializer::ToJSON(
      gd::SerializerElement(gd::SceneNameMangler::Get()
                                ->GetMangledSceneName(project.GetName())
                                .LowerCase()
                                .FindAndReplace(" ", "-")));

  {
    gd::String str =
        fs.ReadFile(gdjsRoot + "/Runtime/Electron/package.json")
            .FindAndReplace("\"GDJS_GAME_NAME\"", jsonName)
            .FindAndReplace("\"GDJS_GAME_AUTHOR\"", jsonAuthor)
            .FindAndReplace("\"GDJS_GAME_VERSION\"", jsonVersion)
            .FindAndReplace("\"GDJS_GAME_MANGLED_NAME\"", jsonMangledName);

    if (!fs.WriteToFile(exportDir + "/package.json", str)) {
      lastError = "Unable to write Electron package.json file.";
      return false;
    }
  }

  {
    gd::String str =
        fs.ReadFile(gdjsRoot + "/Runtime/Electron/main.js")
            .FindAndReplace(
                "800 /*GDJS_WINDOW_WIDTH*/",
                gd::String::From<int>(project.GetGameResolutionWidth()))
            .FindAndReplace(
                "600 /*GDJS_WINDOW_HEIGHT*/",
                gd::String::From<int>(project.GetGameResolutionHeight()))
            .FindAndReplace("\"GDJS_GAME_NAME\"", jsonName);

    if (!fs.WriteToFile(exportDir + "/main.js", str)) {
      lastError = "Unable to write Electron main.js file.";
      return false;
    }
  }

  auto &platformSpecificAssets = project.GetPlatformSpecificAssets();
  auto &resourceManager = project.GetResourcesManager();

  gd::String iconFilename =
      resourceManager
          .GetResource(platformSpecificAssets.Get("desktop", "icon-512"))
          .GetFile();
  auto projectDirectory = gd::AbstractFileSystem::NormalizeSeparator(
      fs.DirNameFrom(project.GetProjectFile()));
  fs.MakeAbsolute(iconFilename, projectDirectory);
  fs.MkDir(exportDir + "/buildResources");
  if (fs.FileExists(iconFilename)) {
    fs.CopyFile(iconFilename, exportDir + "/buildResources/icon.png");
  }

  return true;
}

bool ExporterHelper::CompleteIndexFile(
    gd::String &str,
    gd::String exportDir,
    const std::vector<gd::String> &includesFiles,
    gd::String additionalSpec) {
  if (additionalSpec.empty()) additionalSpec = "{}";

  gd::String codeFilesIncludes;
  for (std::vector<gd::String>::const_iterator it = includesFiles.begin();
       it != includesFiles.end();
       ++it) {
    gd::String scriptSrc = "";
    if (fs.IsAbsolute(*it)) {
      // Most of the time, script source are file paths relative to GDJS root or
      // have been copied in the output directory, so they are relative. It's
      // still useful to test here for absolute files as the exporter could be
      // configured with a file system dealing with URL.
      scriptSrc = *it;
    } else {
      if (!fs.FileExists(exportDir + "/" + *it)) {
        std::cout << "Warning: Unable to find " << exportDir + "/" + *it << "."
                  << std::endl;
        continue;
      }

      scriptSrc = exportDir + "/" + *it;
      fs.MakeRelative(scriptSrc, exportDir);
    }

    codeFilesIncludes += "\t<script src=\"" + scriptSrc +
                         "\" crossorigin=\"anonymous\"></script>\n";
  }

  str = str.FindAndReplace("/* GDJS_CUSTOM_STYLE */", "")
            .FindAndReplace("<!-- GDJS_CUSTOM_HTML -->", "")
            .FindAndReplace("<!-- GDJS_CODE_FILES -->", codeFilesIncludes)
            .FindAndReplace("{}/*GDJS_ADDITIONAL_SPEC*/", additionalSpec);

  return true;
}

void ExporterHelper::AddLibsInclude(bool pixiRenderers,
                                    bool cocosRenderers,
                                    bool websocketDebuggerClient,
                                    std::vector<gd::String> &includesFiles) {
  // First, do not forget common includes (they must be included before events
  // generated code files).
  InsertUnique(includesFiles, "libs/jshashtable.js");
  InsertUnique(includesFiles, "gd.js");
  InsertUnique(includesFiles, "gd-splash-image.js");
  InsertUnique(includesFiles, "libs/hshg.js");
  InsertUnique(includesFiles, "libs/rbush.js");
  InsertUnique(includesFiles, "inputmanager.js");
  InsertUnique(includesFiles, "jsonmanager.js");
  InsertUnique(includesFiles, "timemanager.js");
  InsertUnique(includesFiles, "runtimeobject.js");
  InsertUnique(includesFiles, "profiler.js");
  InsertUnique(includesFiles, "runtimescene.js");
  InsertUnique(includesFiles, "scenestack.js");
  InsertUnique(includesFiles, "polygon.js");
  InsertUnique(includesFiles, "force.js");
  InsertUnique(includesFiles, "layer.js");
  InsertUnique(includesFiles, "timer.js");
  InsertUnique(includesFiles, "runtimegame.js");
  InsertUnique(includesFiles, "variable.js");
  InsertUnique(includesFiles, "variablescontainer.js");
  InsertUnique(includesFiles, "oncetriggers.js");
  InsertUnique(includesFiles, "runtimebehavior.js");
  InsertUnique(includesFiles, "spriteruntimeobject.js");

  // Common includes for events only.
  InsertUnique(includesFiles, "events-tools/commontools.js");
  InsertUnique(includesFiles, "events-tools/runtimescenetools.js");
  InsertUnique(includesFiles, "events-tools/inputtools.js");
  InsertUnique(includesFiles, "events-tools/objecttools.js");
  InsertUnique(includesFiles, "events-tools/cameratools.js");
  InsertUnique(includesFiles, "events-tools/soundtools.js");
  InsertUnique(includesFiles, "events-tools/storagetools.js");
  InsertUnique(includesFiles, "events-tools/stringtools.js");
  InsertUnique(includesFiles, "events-tools/windowtools.js");
  InsertUnique(includesFiles, "events-tools/networktools.js");

  if (websocketDebuggerClient) {
    InsertUnique(includesFiles, "websocket-debugger-client/hot-reloader.js");
    InsertUnique(includesFiles,
                 "websocket-debugger-client/websocket-debugger-client.js");
  }

  if (pixiRenderers) {
    InsertUnique(includesFiles, "pixi-renderers/pixi.js");
    InsertUnique(includesFiles, "pixi-renderers/pixi-filters-tools.js");
    InsertUnique(includesFiles, "pixi-renderers/runtimegame-pixi-renderer.js");
    InsertUnique(includesFiles, "pixi-renderers/runtimescene-pixi-renderer.js");
    InsertUnique(includesFiles, "pixi-renderers/layer-pixi-renderer.js");
    InsertUnique(includesFiles, "pixi-renderers/pixi-image-manager.js");
    InsertUnique(includesFiles,
                 "pixi-renderers/spriteruntimeobject-pixi-renderer.js");
    InsertUnique(includesFiles,
                 "pixi-renderers/loadingscreen-pixi-renderer.js");
    InsertUnique(includesFiles, "howler-sound-manager/howler.min.js");
    InsertUnique(includesFiles, "howler-sound-manager/howler-sound-manager.js");
    InsertUnique(includesFiles,
                 "fontfaceobserver-font-manager/fontfaceobserver.js");
    InsertUnique(
        includesFiles,
        "fontfaceobserver-font-manager/fontfaceobserver-font-manager.js");
  }

  if (cocosRenderers) {
    InsertUnique(includesFiles, "cocos-renderers/cocos-director-manager.js");
    InsertUnique(includesFiles, "cocos-renderers/cocos-image-manager.js");
    InsertUnique(includesFiles, "cocos-renderers/cocos-tools.js");
    InsertUnique(includesFiles, "cocos-renderers/layer-cocos-renderer.js");
    InsertUnique(includesFiles,
                 "cocos-renderers/loadingscreen-cocos-renderer.js");
    InsertUnique(includesFiles,
                 "cocos-renderers/runtimegame-cocos-renderer.js");
    InsertUnique(includesFiles,
                 "cocos-renderers/runtimescene-cocos-renderer.js");
    InsertUnique(includesFiles,
                 "cocos-renderers/spriteruntimeobject-cocos-renderer.js");
    InsertUnique(includesFiles, "cocos-sound-manager/cocos-sound-manager.js");
    InsertUnique(includesFiles,
                 "fontfaceobserver-font-manager/fontfaceobserver.js");
    InsertUnique(
        includesFiles,
        "fontfaceobserver-font-manager/fontfaceobserver-font-manager.js");
  }
}

void ExporterHelper::RemoveIncludes(bool pixiRenderers,
                                    bool cocosRenderers,
                                    std::vector<gd::String> &includesFiles) {
  if (pixiRenderers) {
    for (size_t i = 0; i < includesFiles.size();) {
      const gd::String &includeFile = includesFiles[i];
      if (includeFile.find("pixi-renderer") != gd::String::npos ||
          includeFile.find("pixi-filter") != gd::String::npos)
        includesFiles.erase(includesFiles.begin() + i);
      else
        ++i;
    }
  }
  if (cocosRenderers) {
    for (size_t i = 0; i < includesFiles.size();) {
      const gd::String &includeFile = includesFiles[i];
      if (includeFile.find("cocos-renderer") != gd::String::npos ||
          includeFile.find("cocos-shader") != gd::String::npos)
        includesFiles.erase(includesFiles.begin() + i);
      else
        ++i;
    }
  }
}

bool ExporterHelper::ExportEffectIncludes(
    gd::Project &project, std::vector<gd::String> &includesFiles) {
  std::set<gd::String> effectIncludes;

  gd::EffectsCodeGenerator::GenerateEffectsIncludeFiles(
      project.GetCurrentPlatform(), project, effectIncludes);

  for (auto &include : effectIncludes) InsertUnique(includesFiles, include);

  return true;
}

bool ExporterHelper::ExportEventsCode(gd::Project &project,
                                      gd::String outputDir,
                                      std::vector<gd::String> &includesFiles,
                                      bool exportForPreview) {
  fs.MkDir(outputDir);

  for (std::size_t i = 0; i < project.GetLayoutsCount(); ++i) {
    std::set<gd::String> eventsIncludes;
    gd::Layout &layout = project.GetLayout(i);
    LayoutCodeGenerator layoutCodeGenerator(project);
    gd::String eventsOutput = layoutCodeGenerator.GenerateLayoutCompleteCode(
        layout, eventsIncludes, !exportForPreview);
    gd::String filename =
        outputDir + "/" + "code" + gd::String::From(i) + ".js";

    // Export the code
    if (fs.WriteToFile(filename, eventsOutput)) {
      for (auto &include : eventsIncludes) InsertUnique(includesFiles, include);

      InsertUnique(includesFiles, filename);
    } else {
      lastError = _("Unable to write ") + filename;
      return false;
    }
  }

  return true;
}

bool ExporterHelper::ExportExternalSourceFiles(
    gd::Project &project,
    gd::String outputDir,
    std::vector<gd::String> &includesFiles) {
  const auto &allFiles = project.GetAllSourceFiles();
  for (std::size_t i = 0; i < allFiles.size(); ++i) {
    if (!allFiles[i]) continue;
    if (allFiles[i]->GetLanguage() != "Javascript") continue;

    gd::SourceFile &file = *allFiles[i];

    gd::String filename = file.GetFileName();
    fs.MakeAbsolute(filename, fs.DirNameFrom(project.GetProjectFile()));
    gd::String outFilename = "ext-code" + gd::String::From(i) + ".js";
    if (!fs.CopyFile(filename, outputDir + outFilename))
      gd::LogWarning(_("Could not copy external file") + filename);

    InsertUnique(includesFiles, outputDir + outFilename);
  }

  return true;
}

bool ExporterHelper::ExportIncludesAndLibs(
    std::vector<gd::String> &includesFiles,
    gd::String exportDir,
    bool /*minify*/) {
  for (std::vector<gd::String>::iterator include = includesFiles.begin();
       include != includesFiles.end();
       ++include) {
    if (!fs.IsAbsolute(*include)) {
      gd::String source = gdjsRoot + "/Runtime/" + *include;
      if (fs.FileExists(source)) {
        gd::String path = fs.DirNameFrom(exportDir + "/" + *include);
        if (!fs.DirExists(path)) fs.MkDir(path);

        fs.CopyFile(source, exportDir + "/" + *include);

        gd::String relativeInclude = source;
        fs.MakeRelative(relativeInclude, gdjsRoot + "/Runtime/");
        *include = relativeInclude;
      } else {
        std::cout << "Could not find GDJS include file " << *include
                  << std::endl;
      }
    } else {
      // Note: all the code generated from events are generated in another
      // folder and fall in this case:

      if (fs.FileExists(*include)) {
        fs.CopyFile(*include, exportDir + "/" + fs.FileNameFrom(*include));
        *include = fs.FileNameFrom(
            *include);  // Ensure filename is relative to the export dir.
      } else {
        std::cout << "Could not find include file " << *include << std::endl;
      }
    }
  }

  return true;
}

void ExporterHelper::ExportResources(gd::AbstractFileSystem &fs,
                                     gd::Project &project,
                                     gd::String exportDir) {
  gd::ProjectResourcesCopier::CopyAllResourcesTo(
      project, fs, exportDir, true, false, false);
}

void ExporterHelper::AddDeprecatedFontFilesToFontResources(
    gd::AbstractFileSystem &fs,
    gd::ResourcesManager &resourcesManager,
    const gd::String &exportDir,
    gd::String urlPrefix) {
  // Compatibility with GD <= 5.0-beta56
  //
  // Before, fonts were detected by scanning the export folder for .TTF files.
  // Text Object (or anything using a font) was just declaring the font filename
  // as a file (using ArbitraryResourceWorker::ExposeFile) for export.
  //
  // To still support this, the time everything is migrated to using font
  // resources, we manually declare font resources for each ".TTF" file, using
  // the name of the file as the resource name.
  std::vector<gd::String> ttfFiles = fs.ReadDir(exportDir, ".TTF");
  for (std::size_t i = 0; i < ttfFiles.size(); ++i) {
    gd::String relativeFile = ttfFiles[i];
    fs.MakeRelative(relativeFile, exportDir);

    // Create a resource named like the file (to emulate the old behavior).
    gd::FontResource fontResource;
    fontResource.SetName(relativeFile);
    fontResource.SetFile(urlPrefix + relativeFile);

    // Note that if a resource with this name already exists, it won't be
    // overwritten - which is expected.
    resourcesManager.AddResource(fontResource);
  }
  // end of compatibility code
}

}  // namespace gdjs
