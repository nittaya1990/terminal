// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CascadiaSettings.h"

#include <LibraryResources.h>
#include <fmt/chrono.h>
#include <shlobj.h>
#include <til/latch.h>

#include "AzureCloudShellGenerator.h"
#include "PowershellCoreProfileGenerator.h"
#include "WslDistroGenerator.h"

// The following files are generated at build time into the "Generated Files" directory.
// defaults(-universal).h is a file containing the default json settings in a std::string_view.
#include "defaults.h"
#include "defaults-universal.h"
// userDefault.h is like the above, but with a default template for the user's settings.json.
#include <LegacyProfileGeneratorNamespaces.h>

#include "userDefaults.h"

#include "ApplicationState.h"
#include "FileUtils.h"

using namespace winrt::Microsoft::Terminal::Settings::Model::implementation;
using namespace ::Microsoft::Console;
using namespace ::Microsoft::Terminal::Settings::Model;

static constexpr std::wstring_view SettingsFilename{ L"settings.json" };
static constexpr std::wstring_view DefaultsFilename{ L"defaults.json" };

static constexpr std::string_view ProfilesKey{ "profiles" };
static constexpr std::string_view DefaultSettingsKey{ "defaults" };
static constexpr std::string_view ProfilesListKey{ "list" };
static constexpr std::string_view SchemesKey{ "schemes" };
static constexpr std::string_view NameKey{ "name" };
static constexpr std::string_view UpdatesKey{ "updates" };
static constexpr std::string_view GuidKey{ "guid" };

static constexpr std::wstring_view jsonExtension{ L".json" };
static constexpr std::wstring_view FragmentsSubDirectory{ L"\\Fragments" };
static constexpr std::wstring_view FragmentsPath{ L"\\Microsoft\\Windows Terminal\\Fragments" };

static constexpr std::wstring_view AppExtensionHostName{ L"com.microsoft.windows.terminal.settings" };

// make sure this matches defaults.json.
static constexpr winrt::guid DEFAULT_WINDOWS_POWERSHELL_GUID{ 0x61c54bbd, 0xc2c6, 0x5271, { 0x96, 0xe7, 0x00, 0x9a, 0x87, 0xff, 0x44, 0xbf } };
static constexpr winrt::guid DEFAULT_COMMAND_PROMPT_GUID{ 0x0caa0dad, 0x35be, 0x5f56, { 0xa8, 0xff, 0xaf, 0xce, 0xee, 0xaa, 0x61, 0x01 } };

// Function Description:
// - Extracting the value from an async task (like talking to the app catalog) when we are on the
//   UI thread causes C++/WinRT to complain quite loudly (and halt execution!)
//   This templated function extracts the result from a task with chicanery.
template<typename TTask>
static auto extractValueFromTaskWithoutMainThreadAwait(TTask&& task) -> decltype(task.get())
{
    std::optional<decltype(task.get())> finalVal;
    til::latch latch{ 1 };

    const auto _ = [&]() -> winrt::fire_and_forget {
        co_await winrt::resume_background();
        finalVal.emplace(co_await task);
        latch.count_down();
    }();

    latch.wait();
    return finalVal.value();
}

static std::pair<size_t, size_t> lineAndColumnFromPosition(const std::string_view string, const size_t position)
{
    size_t line = 1;
    size_t pos = 0;

    for (;;)
    {
        const auto p = string.find('\n', pos);
        if (p >= position)
        {
            break;
        }

        pos = p + 1;
        line++;
    }

    return { line, position - pos + 1 };
}

static void rethrowSerializationExceptionWithLocationInfo(const JsonUtils::DeserializationError& e, std::string_view settingsString)
{
    static constexpr std::string_view basicHeader{ "* Line {line}, Column {column}\n{message}" };
    static constexpr std::string_view keyedHeader{ "* Line {line}, Column {column} ({key})\n{message}" };

    std::string jsonValueAsString{ "array or object" };
    try
    {
        jsonValueAsString = e.jsonValue.asString();
        if (e.jsonValue.isString())
        {
            jsonValueAsString = fmt::format("\"{}\"", jsonValueAsString);
        }
    }
    catch (...)
    {
        // discard: we're in the middle of error handling
    }

    auto msg = fmt::format("  Have: {}\n  Expected: {}", jsonValueAsString, e.expectedType);

    auto [l, c] = lineAndColumnFromPosition(settingsString, static_cast<size_t>(e.jsonValue.getOffsetStart()));
    msg = fmt::format((e.key ? keyedHeader : basicHeader),
                      fmt::arg("line", l),
                      fmt::arg("column", c),
                      fmt::arg("key", e.key.value_or("")),
                      fmt::arg("message", msg));
    throw SettingsTypedDeserializationException{ msg };
}

static Json::Value parseJSON(const std::string_view& content)
{
    Json::Value json;
    std::string errs;
    const std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder::CharReaderBuilder().newCharReader() };

    if (!reader->parse(content.data(), content.data() + content.size(), &json, &errs))
    {
        throw winrt::hresult_error(WEB_E_INVALID_JSON_STRING, winrt::to_hstring(errs));
    }

    return json;
}

static const Json::Value& getJSONValue(const Json::Value& json, const std::string_view& key) noexcept
{
    const auto found = json.find(key.data(), key.data() + key.size());
    return found ? *found : Json::Value::nullSingleton();
}

// Function Description:
// - Given a json serialization of a profile, this function will determine
//   whether it is "well-formed". We introduced a bug (GH#9962, fixed in GH#9964)
//   that would result in one or more nameless, guid-less profiles being emitted
//   into the user's settings file. Those profiles would show up in the list as
//   "Default" later.
static bool isValidProfileObject(const Json::Value& profileJson)
{
    return profileJson.isObject() &&
           (profileJson.isMember(NameKey.data(), NameKey.data() + NameKey.size()) || // has a name (can generate a guid)
            profileJson.isMember(GuidKey.data(), GuidKey.data() + GuidKey.size())); // or has a guid
}

ParsedSettings::ParsedSettings(const OriginTag origin, const std::string_view& content)
{
    const auto json = parseJSON(content);

    globals = GlobalAppSettings::FromJson(json);

    if (const auto& schemes = getJSONValue(json, SchemesKey))
    {
        for (const auto& schemeJson : schemes)
        {
            if (schemeJson.isObject() && ColorScheme::ValidateColorScheme(schemeJson))
            {
                globals->AddColorScheme(*ColorScheme::FromJson(schemeJson));
            }
        }
    }

    if (const auto& profilesObject = getJSONValue(json, ProfilesKey))
    {
        const auto& defaultsObject = profilesObject.isObject() ? getJSONValue(profilesObject, DefaultSettingsKey) : Json::Value{ Json::objectValue };
        const auto& profilesArray = profilesObject.isArray() ? profilesObject : (profilesObject.isObject() ? getJSONValue(profilesObject, ProfilesListKey) : Json::Value{ Json::arrayValue });

        if (isValidProfileObject(defaultsObject))
        {
            profileDefaults = Profile::FromJson(defaultsObject);
            // Remove the `guid` member from the default settings.
            // That'll hyper-explode, so just don't let them do that.
            profileDefaults->ClearGuid();
            profileDefaults->Origin(OriginTag::ProfilesDefaults);
        }

        profilesByGuid.reserve(profilesArray.size());

        for (auto profileJson : profilesArray)
        {
            if (isValidProfileObject(profileJson))
            {
                const auto profile = Profile::FromJson(profileJson);
                profile->Origin(origin);

                // Love it.
                if (!profile->HasGuid())
                {
                    profile->Guid(profile->Guid());
                }

                if (auto [it, ok] = profilesByGuid.emplace(profile->Guid(), profile); !ok)
                {
                    it->second->LayerJson(profileJson);
                }
                else
                {
                    profiles.emplace_back(std::move(profile));
                }
            }
        }
    }
}

// Method Description:
// - Creates a CascadiaSettings from whatever's saved on disk, or instantiates
//      a new one with the default values. If we're running as a packaged app,
//      it will load the settings from our packaged localappdata. If we're
//      running as an unpackaged application, it will read it from the path
//      we've set under localappdata.
// - Loads both the settings from the defaults.json and the user's settings.json
// - Also runs and dynamic profile generators. If any of those generators create
//   new profiles, we'll write the user settings back to the file, with the new
//   profiles inserted into their list of profiles.
// Return Value:
// - a unique_ptr containing a new CascadiaSettings object.
winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings CascadiaSettings::LoadAll()
try
{
    const auto settingsString = ReadUTF8FileIfExists(_SettingsPath()).value_or(std::string{});
    const auto settingsStringView = settingsString.empty() ? UserSettingsJson : settingsString;
    bool needToWriteFile = settingsString.empty();

    ParsedSettings defaultSettings{ OriginTag::InBox, DefaultJson };
    ParsedSettings userSettings;

    try
    {
        userSettings = ParsedSettings{ OriginTag::User, settingsStringView };
    }
    catch (const JsonUtils::DeserializationError& e)
    {
        rethrowSerializationExceptionWithLocationInfo(e, settingsStringView);
    }

    // TODO: This std::wstring_view should be replaced with a winrt::hstring
    // winrt::hstring is reference counted and the overhead fairly minimal.
    // Meanwhile the std::wstring_view is rather unsafe code. If anyone modifies the
    // DisabledProfileSources() while we use ignoredNamespaces we'd be accessing invalid memory.
    std::unordered_set<std::wstring_view> ignoredNamespaces;
    if (const auto sources = userSettings.globals->DisabledProfileSources())
    {
        ignoredNamespaces.reserve(sources.Size());
        for (const auto& id : sources)
        {
            ignoredNamespaces.emplace(id);
        }
    }

    std::vector<winrt::com_ptr<Profile>> generatedProfiles;
    std::vector<ParsedSettings> fragments;
    {
#define GENERATORS(X)                 \
    X(PowershellCoreProfileGenerator) \
    X(WslDistroGenerator)             \
    X(AzureCloudShellGenerator)

#define XX(name) name _gen##name;
        GENERATORS(XX)
#undef XX

        const std::array generators{
#define XX(name) static_cast<IDynamicProfileGenerator*>(&_gen##name),
            GENERATORS(XX)
#undef XX
        };

#undef GENERATORS

        for (const auto generator : generators)
        {
            const auto generatorNamespace = generator->GetNamespace();

            if (!ignoredNamespaces.count(generatorNamespace))
            {
                try
                {
                    generator->GenerateProfiles(generatedProfiles);
                }
                CATCH_LOG_MSG("Dynamic Profile Namespace: \"%s\"", generatorNamespace.data());
            }
        }
    }
    {
        for (const auto& rfid : std::array{ FOLDERID_LocalAppData, FOLDERID_ProgramData })
        {
            wil::unique_cotaskmem_string folder;
            THROW_IF_FAILED(SHGetKnownFolderPath(rfid, 0, nullptr, &folder));

            std::wstring fragmentPath{ folder.get() };
            fragmentPath.append(FragmentsPath);

            if (std::filesystem::exists(fragmentPath))
            {
                for (const auto& fragmentExtFolder : std::filesystem::directory_iterator(fragmentPath))
                {
                    const auto filename = fragmentExtFolder.path().filename();
                    const auto& source = filename.native();

                    if (!ignoredNamespaces.count(std::wstring_view{ source }) && std::filesystem::is_directory(fragmentExtFolder))
                    {
                        for (const auto& fragmentExt : std::filesystem::directory_iterator(fragmentExtFolder.path()))
                        {
                            if (fragmentExt.path().extension() == jsonExtension)
                            {
                                try
                                {
                                    const auto content = ReadUTF8File(fragmentExt.path());
                                    const auto& settings = fragments.emplace_back(OriginTag::Fragment, content);

                                    for (const auto& profile : settings.profiles)
                                    {
                                        profile->Source(winrt::hstring{ source });
                                    }
                                }
                                CATCH_LOG();
                            }
                        }
                    }
                }
            }
        }

        // Search through app extensions
        // Gets the catalog of extensions with the name "com.microsoft.windows.terminal.settings"
        const auto catalog = Windows::ApplicationModel::AppExtensions::AppExtensionCatalog::Open(AppExtensionHostName);
        const auto extensions = extractValueFromTaskWithoutMainThreadAwait(catalog.FindAllAsync());

        for (const auto& ext : extensions)
        {
            const auto packageName = ext.Package().Id().FamilyName();
            if (ignoredNamespaces.count(std::wstring_view{ packageName }))
            {
                continue;
            }

            // Likewise, getting the public folder from an extension is an async operation.
            auto foundFolder = extractValueFromTaskWithoutMainThreadAwait(ext.GetPublicFolderAsync());
            if (!foundFolder)
            {
                continue;
            }

            // the StorageFolder class has its own methods for obtaining the files within the folder
            // however, all those methods are Async methods
            // you may have noticed that we need to resort to clunky implementations for async operations
            // (they are in extractValueFromTaskWithoutMainThreadAwait)
            // so for now we will just take the folder path and access the files that way
            std::wstring path{ foundFolder.Path() };
            path.append(FragmentsSubDirectory);

            if (std::filesystem::is_directory(path))
            {
                for (const auto& fragmentExt : std::filesystem::directory_iterator(path))
                {
                    if (fragmentExt.path().extension() == jsonExtension)
                    {
                        try
                        {
                            const auto content = ReadUTF8File(fragmentExt.path());
                            const auto& settings = fragments.emplace_back(OriginTag::Fragment, content);

                            for (const auto& profile : settings.profiles)
                            {
                                profile->Source(packageName);
                            }
                        }
                        CATCH_LOG();
                    }
                }
            }
        }
    }

    if (settingsString.empty())
    {
        // A new settings.json defaults to a PowerShell 7+ profile, if one was generated,
        // or falls back to the always existing PowerShell 5 profile otherwise.
        {
            const auto preferredPowershellProfile = PowershellCoreProfileGenerator::GetPreferredPowershellProfileName();
            auto guid = DEFAULT_WINDOWS_POWERSHELL_GUID;

            for (const auto& profile : generatedProfiles)
            {
                if (profile->Name() == preferredPowershellProfile)
                {
                    guid = profile->Guid();
                    break;
                }
            }

            userSettings.globals->DefaultProfile(guid);
        }

        // cmd.exe gets a localized name
        {
            for (const auto& profile : userSettings.profiles)
            {
                if (profile->Guid() == DEFAULT_COMMAND_PROMPT_GUID)
                {
                    profile->Name(RS_(L"CommandPromptDisplayName"));
                    break;
                }
            }
        }
    }

    for (const auto& generatedProfile : generatedProfiles)
    {
        const auto it = userSettings.profilesByGuid.find(generatedProfile->Guid());
        if (it != userSettings.profilesByGuid.end())
        {
            const auto& profile = it->second;
            const auto generatorSource = generatedProfile->Source();
            const auto userSource = profile->Source();

            if (generatorSource == userSource ||
                // These generators were built into the code before we
                // had a proper concept of a dynamic profile source.
                // As such, these profiles will exist in user's settings without a `source` attribute,
                // and we'll need to make sure to handle layering them specially.
                (userSource.empty() &&
                 (generatorSource == WslGeneratorNamespace ||
                  generatorSource == AzureGeneratorNamespace ||
                  generatorSource == PowershellCoreGeneratorNamespace)))
            {
                profile->InsertParent(generatedProfile);
                continue;
            }
        }

        auto profile = generatedProfile->CreateChild();
        profile->Guid(profile->Guid());
        userSettings.profiles.emplace_back(std::move(profile));
    }

    const auto settings = winrt::make_self<CascadiaSettings>();
    {
        std::vector<Model::Profile> allProfiles;
        std::vector<Model::Profile> activeProfiles;

        allProfiles.reserve(userSettings.profiles.size());

        for (const auto& profile : userSettings.profiles)
        {
            allProfiles.emplace_back(*profile);
            if (!profile->Hidden())
            {
                activeProfiles.emplace_back(*profile);
            }
        }

        settings->_globals = userSettings.globals;
        settings->_allProfiles = winrt::single_threaded_observable_vector(std::move(allProfiles));
        settings->_activeProfiles = winrt::single_threaded_observable_vector(std::move(activeProfiles));
    }

    // If we created the file, or found new dynamic profiles, write the user
    // settings string back to the file.
    if (needToWriteFile)
    {
        try
        {
            settings->WriteSettingsToDisk();
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            settings->AppendWarning(SettingsLoadWarnings::FailedToWriteToSettings);
        }
    }

    // If this throws, the app will catch it and use the default settings
    settings->_ValidateSettings();

    return *settings;
}
catch (const SettingsException& ex)
{
    auto settings{ winrt::make_self<implementation::CascadiaSettings>() };
    settings->_loadError = ex.Error();
    return *settings;
}
catch (const SettingsTypedDeserializationException& e)
{
    auto settings{ winrt::make_self<implementation::CascadiaSettings>() };
    std::string_view what{ e.what() };
    settings->_deserializationErrorMessage = til::u8u16(what);
    return *settings;
}

// Function Description:
// - Loads a batch of settings curated for the Universal variant of the terminal app
// Arguments:
// - <none>
// Return Value:
// - a unique_ptr to a CascadiaSettings with the connection types and settings for Universal terminal
winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings CascadiaSettings::LoadUniversal()
{
    ParsedSettings parsed{ OriginTag::InBox, DefaultUniversalJson };

    const auto resultPtr{ winrt::make_self<CascadiaSettings>() };
    resultPtr->_globals = parsed.globals;
    resultPtr->_allProfiles = parsed.fuckyou1();

    resultPtr->_UpdateActiveProfiles();
    resultPtr->_ResolveDefaultProfile();

    return *resultPtr;
}

// Function Description:
// - Creates a new CascadiaSettings object initialized with settings from the
//   hardcoded defaults.json.
// Arguments:
// - <none>
// Return Value:
// - a unique_ptr to a CascadiaSettings with the settings from defaults.json
winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings CascadiaSettings::LoadDefaults()
{
    ParsedSettings parsed{ OriginTag::InBox, DefaultJson };

    const auto resultPtr{ winrt::make_self<CascadiaSettings>() };
    resultPtr->_globals = parsed.globals;
    resultPtr->_allProfiles = parsed.fuckyou1();

    resultPtr->_UpdateActiveProfiles();
    resultPtr->_ResolveDefaultProfile();

    return *resultPtr;
}

// Method Description:
// - Create a new instance of this class from a serialized JsonObject.
// Arguments:
// - json: an object which should be a serialization of a CascadiaSettings object.
// Return Value:
// - a new CascadiaSettings instance created from the values in `json`
winrt::com_ptr<CascadiaSettings> CascadiaSettings::FromJson(const Json::Value& json)
{
    auto resultPtr = winrt::make_self<CascadiaSettings>();
    resultPtr->LayerJson(json);
    return resultPtr;
}

// Method Description:
// - Layer values from the given json object on top of the existing properties
//   of this object. For any keys we're expecting to be able to parse in the
//   given object, we'll parse them and replace our settings with values from
//   the new json object. Properties that _aren't_ in the json object will _not_
//   be replaced.
// Arguments:
// - json: an object which should be a partial serialization of a CascadiaSettings object.
// Return Value:
// <none>
void CascadiaSettings::LayerJson(const Json::Value& json)
{
    // add a new inheritance layer, and apply json values to child
    _globals = _globals->CreateChild();
    _globals->LayerJson(json);

    if (auto schemes{ json[JsonKey(SchemesKey)] })
    {
        for (auto schemeJson : schemes)
        {
            if (schemeJson.isObject())
            {
                _LayerOrCreateColorScheme(schemeJson);
            }
        }
    }

    for (auto profileJson : _GetProfilesJsonObject(json))
    {
        if (profileJson.isObject() && isValidProfileObject(profileJson))
        {
            _LayerOrCreateProfile(profileJson);
        }
    }
}

// Method Description:
// - Given a partial json serialization of a Profile object, either layers that
//   json on a matching Profile we already have, or creates a new Profile
//   object from those settings.
// - For profiles that were created from a dynamic profile source, they'll have
//   both a guid and source guid that must both match. If a user profile with a
//   source set does not find a matching profile at load time, the profile
//   should be ignored.
// Arguments:
// - json: an object which may be a partial serialization of a Profile object.
// Return Value:
// - <none>
void CascadiaSettings::_LayerOrCreateProfile(const Json::Value& profileJson)
{
    // Layer the json on top of an existing profile, if we have one:
    winrt::com_ptr<implementation::Profile> profile{ nullptr };
    auto profileIndex{ _FindMatchingProfileIndex(profileJson) };
    if (profileIndex)
    {
        auto parentProj{ _allProfiles.GetAt(*profileIndex) };
        auto parent{ winrt::get_self<Profile>(parentProj) };

        if (_userDefaultProfileSettings)
        {
            // We don't actually need to CreateChild() here.
            // When we loaded Profile.Defaults, we created an empty child already.
            // So this just populates the empty child
            parent->LayerJson(profileJson);
            profile.copy_from(parent);
        }
        else
        {
            // otherwise, add a new inheritance layer
            auto childImpl{ parent->CreateChild() };
            childImpl->LayerJson(profileJson);

            // replace parent in _profiles with child
            _allProfiles.SetAt(*profileIndex, *childImpl);
            profile = std::move(childImpl);
        }
    }
    else
    {
        // If this JSON represents a dynamic profile, we _shouldn't_ create the
        // profile here. We only want to create profiles for profiles without a
        // `source`. Dynamic profiles _must_ be layered on an existing profile.
        if (!Profile::IsDynamicProfileObject(profileJson))
        {
            profile = winrt::make_self<Profile>();

            // GH#2325: If we have a set of default profile settings, set that as my parent.
            // We _won't_ have these settings yet for defaults, dynamic profiles.
            if (_userDefaultProfileSettings)
            {
                Profile::InsertParentHelper(profile, _userDefaultProfileSettings, 0);
            }

            profile->LayerJson(profileJson);
            _allProfiles.Append(*profile);
        }
    }

    if (profile && _userDefaultProfileSettings)
    {
        // If we've loaded defaults{} we're in the "user settings" phase for sure
        profile->Origin(OriginTag::User);
    }
}

// Method Description:
// - Finds a profile from our list of profiles that matches the given json
//   object. Uses Profile::ShouldBeLayered to determine if the Json::Value is a
//   match or not. This method should be used to find a profile to layer the
//   given settings upon.
// - Returns nullptr if no such match exists.
// Arguments:
// - json: an object which may be a partial serialization of a Profile object.
// Return Value:
// - a Profile that can be layered with the given json object, iff such a
//   profile exists.
winrt::com_ptr<Profile> CascadiaSettings::_FindMatchingProfile(const Json::Value& profileJson)
{
    auto index{ _FindMatchingProfileIndex(profileJson) };
    if (index)
    {
        auto profile{ _allProfiles.GetAt(*index) };
        auto profileImpl{ winrt::get_self<Profile>(profile) };
        return profileImpl->get_strong();
    }
    return nullptr;
}

// Method Description:
// - Finds a profile from our list of profiles that matches the given json
//   object. Uses Profile::ShouldBeLayered to determine if the Json::Value is a
//   match or not. This method should be used to find a profile to layer the
//   given settings upon.
// - Returns nullopt if no such match exists.
// Arguments:
// - json: an object which may be a partial serialization of a Profile object.
// Return Value:
// - The index for the matching Profile, iff it exists. Otherwise, nullopt.
std::optional<uint32_t> CascadiaSettings::_FindMatchingProfileIndex(const Json::Value& profileJson)
{
    for (uint32_t i = 0; i < _allProfiles.Size(); ++i)
    {
        const auto profile{ _allProfiles.GetAt(i) };
        const auto profileImpl = winrt::get_self<Profile>(profile);
        if (profileImpl->ShouldBeLayered(profileJson))
        {
            return i;
        }
    }
    return std::nullopt;
}

// Method Description:
// - Given a partial json serialization of a ColorScheme object, either layers that
//   json on a matching ColorScheme we already have, or creates a new ColorScheme
//   object from those settings.
// Arguments:
// - json: an object which should be a partial serialization of a ColorScheme object.
// Return Value:
// - <none>
void CascadiaSettings::_LayerOrCreateColorScheme(const Json::Value& schemeJson)
{
    // Layer the json on top of an existing profile, if we have one:
    auto pScheme = _FindMatchingColorScheme(schemeJson);
    if (pScheme)
    {
        pScheme->LayerJson(schemeJson);
    }
    else
    {
        const auto scheme = ColorScheme::FromJson(schemeJson);
        _globals->AddColorScheme(*scheme);
    }
}

// Method Description:
// - Finds a color scheme from our list of color schemes that matches the given
//   json object. Uses ColorScheme::GetNameFromJson to find the name and then
//   performs a lookup in the global map. This method should be used to find a
//   color scheme to layer the given settings upon.
// - Returns nullptr if no such match exists.
// Arguments:
// - json: an object which should be a partial serialization of a ColorScheme object.
// Return Value:
// - a ColorScheme that can be layered with the given json object, iff such a
//   color scheme exists.
winrt::com_ptr<ColorScheme> CascadiaSettings::_FindMatchingColorScheme(const Json::Value& schemeJson)
{
    if (auto schemeName = ColorScheme::GetNameFromJson(schemeJson))
    {
        if (auto scheme{ _globals->ColorSchemes().TryLookup(*schemeName) })
        {
            return winrt::get_self<ColorScheme>(scheme)->get_strong();
        }
    }
    return nullptr;
}

// Method Description:
// - Returns the path of the settings.json file.
// Arguments:
// - <none>
// Return Value:
// - Returns a path in 80% of cases. I measured!
const std::filesystem::path& CascadiaSettings::_SettingsPath()
{
    static const auto path = GetBaseSettingsPath() / SettingsFilename;
    return path;
}

// function Description:
// - Returns the full path to the settings file, either within the application
//   package, or in its unpackaged location. This path is under the "Local
//   AppData" folder, so it _doesn't_ roam to other machines.
// - If the application is unpackaged,
//   the file will end up under e.g. C:\Users\admin\AppData\Local\Microsoft\Windows Terminal\settings.json
// Arguments:
// - <none>
// Return Value:
// - the full path to the settings file
winrt::hstring CascadiaSettings::SettingsPath()
{
    return winrt::hstring{ _SettingsPath().wstring() };
}

winrt::hstring CascadiaSettings::DefaultSettingsPath()
{
    // Both of these posts suggest getting the path to the exe, then removing
    // the exe's name to get the package root:
    // * https://blogs.msdn.microsoft.com/appconsult/2017/06/23/accessing-to-the-files-in-the-installation-folder-in-a-desktop-bridge-application/
    // * https://blogs.msdn.microsoft.com/appconsult/2017/03/06/handling-data-in-a-converted-desktop-app-with-the-desktop-bridge/
    //
    // This would break if we ever moved our exe out of the package root.
    // HOWEVER, if we try to look for a defaults.json that's simply in the same
    // directory as the exe, that will work for unpackaged scenarios as well. So
    // let's try that.

    std::wstring exePathString;
    THROW_IF_FAILED(wil::GetModuleFileNameW(nullptr, exePathString));

    std::filesystem::path path{ exePathString };
    path.replace_filename(DefaultsFilename);
    return winrt::hstring{ path.wstring() };
}

// Function Description:
// - Gets the object in the given JSON object under the "profiles" key. Returns
//   null if there's no "profiles" key.
// Arguments:
// - json: the json object to get the profiles from.
// Return Value:
// - the Json::Value representing the profiles property from the given object
const Json::Value& CascadiaSettings::_GetProfilesJsonObject(const Json::Value& json)
{
    const auto& profilesProperty = json[JsonKey(ProfilesKey)];
    return profilesProperty.isArray() ?
               profilesProperty :
               profilesProperty[JsonKey(ProfilesListKey)];
}

// Method Description:
// - Write the current state of CascadiaSettings to our settings file
// - Create a backup file with the current contents, if one does not exist
// - Persists the default terminal handler choice to the registry
// Arguments:
// - <none>
// Return Value:
// - <none>
void CascadiaSettings::WriteSettingsToDisk() const
{
    const auto settingsPath = _SettingsPath();

    {
        // create a timestamped backup file
        const auto backupSettingsPath = fmt::format(L"{}.{:%Y-%m-%dT%H-%M-%S}.backup", settingsPath.native(), fmt::localtime(std::time(nullptr)));
        LOG_IF_WIN32_BOOL_FALSE(CopyFileW(settingsPath.c_str(), backupSettingsPath.c_str(), TRUE));
    }

    // write current settings to current settings file
    Json::StreamWriterBuilder wbuilder;
    wbuilder.settings_["indentation"] = "    ";
    wbuilder.settings_["enableYAMLCompatibility"] = true; // suppress spaces around colons

    const auto styledString{ Json::writeString(wbuilder, ToJson()) };
    WriteUTF8FileAtomic(settingsPath, styledString);

    // Persists the default terminal choice
    //
    // GH#10003 - Only do this if _currentDefaultTerminal was actually
    // initialized. It's only initialized when Launch.cpp calls
    // `CascadiaSettings::RefreshDefaultTerminals`. We really don't need it
    // otherwise.
    if (_currentDefaultTerminal)
    {
        Model::DefaultTerminal::Current(_currentDefaultTerminal);
    }
}

// Method Description:
// - Create a new serialized JsonObject from an instance of this class
// Arguments:
// - <none>
// Return Value:
// the JsonObject representing this instance
Json::Value CascadiaSettings::ToJson() const
{
    // top-level json object
    Json::Value json{ _globals->ToJson() };
    json["$help"] = "https://aka.ms/terminal-documentation";
    json["$schema"] = "https://aka.ms/terminal-profiles-schema";

    // "profiles" will always be serialized as an object
    Json::Value profiles{ Json::ValueType::objectValue };
    profiles[JsonKey(DefaultSettingsKey)] = _userDefaultProfileSettings ? _userDefaultProfileSettings->ToJson() :
                                                                          Json::ValueType::objectValue;
    Json::Value profilesList{ Json::ValueType::arrayValue };
    for (const auto& entry : _allProfiles)
    {
        if (!entry.Deleted())
        {
            const auto prof{ winrt::get_self<implementation::Profile>(entry) };
            profilesList.append(prof->ToJson());
        }
    }
    profiles[JsonKey(ProfilesListKey)] = profilesList;
    json[JsonKey(ProfilesKey)] = profiles;

    // TODO GH#8100:
    // "schemes" will be an accumulation of _all_ the color schemes
    // including all of the ones from defaults.json
    Json::Value schemes{ Json::ValueType::arrayValue };
    for (const auto& entry : _globals->ColorSchemes())
    {
        const auto scheme{ winrt::get_self<implementation::ColorScheme>(entry.Value()) };
        schemes.append(scheme->ToJson());
    }
    json[JsonKey(SchemesKey)] = schemes;

    return json;
}
