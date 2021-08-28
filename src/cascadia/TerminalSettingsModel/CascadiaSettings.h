/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- CascadiaSettings.h

Abstract:
- This class acts as the container for all app settings. It's composed of two
        parts: Globals, which are app-wide settings, and Profiles, which contain
        a set of settings that apply to a single instance of the terminal.
  Also contains the logic for serializing and deserializing this object.

Author(s):
- Mike Griese - March 2019

--*/
#pragma once

#include "CascadiaSettings.g.h"

#include "GlobalAppSettings.h"
#include "TerminalWarnings.h"
#include "IDynamicProfileGenerator.h"

#include "Profile.h"
#include "ColorScheme.h"

// fwdecl unittest classes
namespace SettingsModelLocalTests
{
    class SerializationTests;
    class DeserializationTests;
    class ProfileTests;
    class ColorSchemeTests;
    class KeyBindingsTests;
};
namespace TerminalAppUnitTests
{
    class DynamicProfileTests;
    class JsonTests;
};

namespace Microsoft::Terminal::Settings::Model
{
    class SettingsTypedDeserializationException;
};

class Microsoft::Terminal::Settings::Model::SettingsTypedDeserializationException final : public std::runtime_error
{
public:
    SettingsTypedDeserializationException(const std::string_view description) :
        runtime_error(description.data()) {}
};

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    struct ParsedSettings
    {
        ParsedSettings() noexcept = default;
        explicit ParsedSettings(OriginTag origin, const std::string_view& content);

        winrt::Windows::Foundation::Collections::IObservableVector<Model::Profile> fuckyou1() const
        {
            std::vector<Model::Profile> vec;
            vec.reserve(profiles.size());

            for (const auto& p : profiles)
            {
                vec.emplace_back(*p);
            }

            return winrt::single_threaded_observable_vector(std::move(vec));
        }

        winrt::com_ptr<GlobalAppSettings> globals;
        winrt::com_ptr<Profile> profileDefaults;
        std::vector<winrt::com_ptr<Profile>> profiles;
        std::unordered_map<winrt::guid, winrt::com_ptr<Profile>> profilesByGuid;
    };

    struct CascadiaSettings : CascadiaSettingsT<CascadiaSettings>
    {
    public:
        CascadiaSettings() = default;
        explicit CascadiaSettings(std::string_view json);
        explicit CascadiaSettings(std::wstring_view json);
        explicit CascadiaSettings(const winrt::hstring& json);

        Model::CascadiaSettings Copy() const;

        static Model::CascadiaSettings LoadDefaults();
        static Model::CascadiaSettings LoadAll();
        static Model::CascadiaSettings LoadUniversal();

        Model::GlobalAppSettings GlobalSettings() const;
        Windows::Foundation::Collections::IObservableVector<Model::Profile> AllProfiles() const noexcept;
        Windows::Foundation::Collections::IObservableVector<Model::Profile> ActiveProfiles() const noexcept;
        Model::ActionMap ActionMap() const noexcept;

        static com_ptr<CascadiaSettings> FromJson(const Json::Value& json);
        void LayerJson(const Json::Value& json);

        void WriteSettingsToDisk() const;
        Json::Value ToJson() const;

        static hstring SettingsPath();
        static hstring DefaultSettingsPath();
        Model::Profile ProfileDefaults() const;

        static winrt::hstring ApplicationDisplayName();
        static winrt::hstring ApplicationVersion();

        Model::Profile CreateNewProfile();
        Model::Profile FindProfile(const guid& profileGuid) const noexcept;
        Model::ColorScheme GetColorSchemeForProfile(const Model::Profile& profile) const;
        void UpdateColorSchemeReferences(const hstring oldName, const hstring newName);

        Windows::Foundation::Collections::IVectorView<SettingsLoadWarnings> Warnings();
        void ClearWarnings();
        void AppendWarning(SettingsLoadWarnings warning);
        Windows::Foundation::IReference<SettingsLoadErrors> GetLoadingError();
        hstring GetSerializationErrorMessage();

        Model::Profile GetProfileForArgs(const Model::NewTerminalArgs& newTerminalArgs) const;

        Model::Profile DuplicateProfile(const Model::Profile& source);
        void RefreshDefaultTerminals();

        static bool IsDefaultTerminalAvailable() noexcept;
        Windows::Foundation::Collections::IObservableVector<Model::DefaultTerminal> DefaultTerminals() const noexcept;
        Model::DefaultTerminal CurrentDefaultTerminal() const noexcept;
        void CurrentDefaultTerminal(Model::DefaultTerminal terminal);

    private:
        com_ptr<GlobalAppSettings> _globals{ winrt::make_self<implementation::GlobalAppSettings>() };
        Windows::Foundation::Collections::IObservableVector<Model::Profile> _allProfiles{ winrt::single_threaded_observable_vector<Model::Profile>() };
        Windows::Foundation::Collections::IObservableVector<Model::Profile> _activeProfiles{ winrt::single_threaded_observable_vector<Model::Profile>() };
        Windows::Foundation::Collections::IVector<Model::SettingsLoadWarnings> _warnings{ winrt::single_threaded_vector<SettingsLoadWarnings>() };
        Windows::Foundation::IReference<SettingsLoadErrors> _loadError;
        hstring _deserializationErrorMessage;
        Windows::Foundation::Collections::IObservableVector<Model::DefaultTerminal> _defaultTerminals{ winrt::single_threaded_observable_vector<Model::DefaultTerminal>() };
        Model::DefaultTerminal _currentDefaultTerminal{ nullptr };
        winrt::com_ptr<Profile> _userDefaultProfileSettings;

        winrt::com_ptr<Profile> _CreateNewProfile(const std::wstring_view& name) const;

        void _LayerOrCreateProfile(const Json::Value& profileJson);
        winrt::com_ptr<implementation::Profile> _FindMatchingProfile(const Json::Value& profileJson);
        std::optional<uint32_t> _FindMatchingProfileIndex(const Json::Value& profileJson);
        void _LayerOrCreateColorScheme(const Json::Value& schemeJson);
        static Json::Value _ParseUtf8JsonString(std::string_view fileData);

        winrt::com_ptr<implementation::ColorScheme> _FindMatchingColorScheme(const Json::Value& schemeJson);
        static const Json::Value& _GetProfilesJsonObject(const Json::Value& json);
        static std::string _ApplyFirstRunChangesToSettingsTemplate(const std::string_view& settingsTemplate);
        void _CopyProfileInheritanceTree(com_ptr<CascadiaSettings>& cloneSettings) const;

        void _ApplyDefaultsFromUserSettings(const Json::Value& userSettings);

        void _LoadDynamicProfiles(const std::unordered_set<std::wstring>& ignoredNamespaces);
        void _LoadFragmentExtensions(const std::unordered_set<std::wstring>& ignoredNamespaces);
        void _ApplyJsonStubsHelper(const std::wstring_view directory, const std::unordered_set<std::wstring>& ignoredNamespaces);
        std::unordered_set<std::string> _AccumulateJsonFilesInDirectory(const std::wstring_view directory);
        void _ParseAndLayerFragmentFiles(const std::unordered_set<std::string> files, const winrt::hstring source);

        static const std::filesystem::path& _SettingsPath();

        std::optional<guid> _GetProfileGuidByName(const hstring) const;
        std::optional<guid> _GetProfileGuidByIndex(std::optional<int> index) const;

        void _ValidateSettings();
        void _ValidateProfilesExist();
        void _ValidateDefaultProfileExists();
        void _ValidateNoDuplicateProfiles();
        void _ResolveDefaultProfile();
        void _UpdateActiveProfiles();
        void _ValidateAllSchemesExist();
        void _ValidateMediaResources();
        void _ValidateKeybindings();
        void _ValidateColorSchemesInCommands();

        bool _HasInvalidColorScheme(const Model::Command& command);

        friend class SettingsModelLocalTests::SerializationTests;
        friend class SettingsModelLocalTests::DeserializationTests;
        friend class SettingsModelLocalTests::ProfileTests;
        friend class SettingsModelLocalTests::ColorSchemeTests;
        friend class SettingsModelLocalTests::KeyBindingsTests;
        friend class TerminalAppUnitTests::DynamicProfileTests;
        friend class TerminalAppUnitTests::JsonTests;
    };
}

namespace winrt::Microsoft::Terminal::Settings::Model::factory_implementation
{
    BASIC_FACTORY(CascadiaSettings);
}
