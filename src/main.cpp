#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr const char* kFileHeader = ">>> FILE ";
constexpr const char* kFileFooter = "<<< END";
constexpr const char* kZshBegin = "# >>> ds-theme >>>";
constexpr const char* kZshEnd = "# <<< ds-theme <<<";

struct FileBlock {
    std::string logicalPath;
    std::string content;
};

struct Options {
    std::filesystem::path configPath;
    std::filesystem::path themePath;
    std::filesystem::path homePath;
    enum class Mode {
        Update,
        EditTheme,
        EditConfig,
    } mode = Mode::Update;
    std::string editor = "nano";
};

struct BundlePaths {
    std::filesystem::path configPath;
    std::filesystem::path themePath;
};

void bootstrapUserBundle(Options& opts) {
    const auto userBundleDir = opts.homePath / ".config" / "ds";
    const auto userConfig = userBundleDir / "configs.ds";
    const auto userTheme = userBundleDir / "themes.ds";

    std::filesystem::create_directories(userBundleDir);

    if (!std::filesystem::exists(userConfig)) {
        if (!std::filesystem::exists(opts.configPath)) {
            throw std::runtime_error("Cannot initialize user config bundle: missing source configs.ds");
        }
        std::filesystem::copy_file(opts.configPath, userConfig, std::filesystem::copy_options::overwrite_existing);
    }

    if (!std::filesystem::exists(userTheme)) {
        if (!std::filesystem::exists(opts.themePath)) {
            throw std::runtime_error("Cannot initialize user config bundle: missing source themes.ds");
        }
        std::filesystem::copy_file(opts.themePath, userTheme, std::filesystem::copy_options::overwrite_existing);
    }

    opts.configPath = userConfig;
    opts.themePath = userTheme;
}

std::string trim(const std::string& input) {
    const std::string whitespace = " \t\r\n";
    const auto start = input.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const auto end = input.find_last_not_of(whitespace);
    return input.substr(start, end - start + 1);
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::filesystem::path executablePath() {
    std::vector<char> buffer(4096);
    const auto len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (len <= 0) {
        throw std::runtime_error("Unable to resolve executable path.");
    }
    buffer[static_cast<size_t>(len)] = '\0';
    return std::filesystem::path(buffer.data());
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Cannot read file: " + path.string());
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeTextFile(const std::filesystem::path& path, const std::string& content) {
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Cannot write file: " + path.string());
    }
    out << content;
    if (!out.good()) {
        throw std::runtime_error("Failed writing file: " + path.string());
    }
}

std::unordered_map<std::string, std::string> parseTheme(const std::filesystem::path& themePath) {
    std::unordered_map<std::string, std::string> theme;
    std::ifstream in(themePath);
    if (!in) {
        throw std::runtime_error("Cannot read theme file: " + themePath.string());
    }

    std::string line;
    size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const auto cleaned = trim(line);
        if (cleaned.empty()) {
            continue;
        }
        const auto eq = cleaned.find('=');
        if (eq == std::string::npos) {
            throw std::runtime_error("Invalid theme entry at line " + std::to_string(lineNo));
        }
        auto key = trim(cleaned.substr(0, eq));
        auto value = trim(cleaned.substr(eq + 1));
        if (key.empty() || value.empty()) {
            throw std::runtime_error("Invalid theme entry at line " + std::to_string(lineNo));
        }
        theme[std::move(key)] = std::move(value);
    }

    if (theme.empty()) {
        throw std::runtime_error("Theme file is empty: " + themePath.string());
    }
    return theme;
}

std::vector<FileBlock> parseConfigs(const std::filesystem::path& configsPath) {
    std::ifstream in(configsPath);
    if (!in) {
        throw std::runtime_error("Cannot read config file: " + configsPath.string());
    }

    std::vector<FileBlock> blocks;
    std::optional<FileBlock> current;
    std::string line;

    while (std::getline(in, line)) {
        if (startsWith(line, kFileHeader)) {
            if (current.has_value()) {
                throw std::runtime_error("Nested file block detected.");
            }
            const auto logical = trim(line.substr(std::string(kFileHeader).size()));
            if (logical.empty()) {
                throw std::runtime_error("Empty file path in config block header.");
            }
            current = FileBlock{logical, ""};
            continue;
        }

        if (line == kFileFooter) {
            if (!current.has_value()) {
                throw std::runtime_error("Unexpected block footer.");
            }
            blocks.push_back(*current);
            current.reset();
            continue;
        }

        if (current.has_value()) {
            current->content += line;
            current->content.push_back('\n');
        }
    }

    if (current.has_value()) {
        throw std::runtime_error("Unclosed config file block.");
    }
    if (blocks.empty()) {
        throw std::runtime_error("No config blocks found in: " + configsPath.string());
    }
    return blocks;
}

std::string applyTheme(const std::string& input, const std::unordered_map<std::string, std::string>& theme) {
    std::string output;
    output.reserve(input.size());

    size_t cursor = 0;
    while (cursor < input.size()) {
        const auto open = input.find("{{", cursor);
        if (open == std::string::npos) {
            output.append(input.substr(cursor));
            break;
        }

        output.append(input.substr(cursor, open - cursor));
        const auto close = input.find("}}", open + 2);
        if (close == std::string::npos) {
            throw std::runtime_error("Unclosed theme token in config content.");
        }

        const auto key = trim(input.substr(open + 2, close - (open + 2)));
        const auto found = theme.find(key);
        if (found == theme.end()) {
            throw std::runtime_error("Missing theme key: " + key);
        }
        output.append(found->second);
        cursor = close + 2;
    }

    return output;
}

std::filesystem::path resolveTarget(const std::filesystem::path& home, const std::string& logicalPath) {
    if (logicalPath == "alacritty/alacritty.toml") {
        return home / ".config" / "alacritty" / "alacritty.toml";
    }
    if (logicalPath == "fuzzel/fuzzel.ini") {
        return home / ".config" / "fuzzel" / "fuzzel.ini";
    }
    if (logicalPath == "mango/autostart.sh") {
        return home / ".config" / "mango" / "autostart.sh";
    }
    if (logicalPath == "mango/config.conf") {
        return home / ".config" / "mango" / "config.conf";
    }
    if (logicalPath == "waybar/config.jsonc") {
        return home / ".config" / "waybar" / "config.jsonc";
    }
    if (logicalPath == "waybar/style.css") {
        return home / ".config" / "waybar" / "style.css";
    }
    if (logicalPath == ".zshrc") {
        return home / ".zshrc";
    }
    throw std::runtime_error("Unsupported target file in configs.ds: " + logicalPath);
}

std::string normalizeWithTrailingNewline(const std::string& content) {
    if (content.empty() || content.back() == '\n') {
        return content;
    }
    return content + '\n';
}

void updateZshrc(const std::filesystem::path& path, const std::string& snippet) {
    std::string existing;
    if (std::filesystem::exists(path)) {
        existing = readTextFile(path);
    }
    existing = normalizeWithTrailingNewline(existing);

    const std::string managed = std::string(kZshBegin) + "\n" + normalizeWithTrailingNewline(snippet) + kZshEnd + "\n";

    const auto beginPos = existing.find(kZshBegin);
    const auto endPos = existing.find(kZshEnd);

    if (beginPos != std::string::npos && endPos != std::string::npos && endPos > beginPos) {
        const auto endAfterMarker = existing.find('\n', endPos);
        const auto replaceEnd = (endAfterMarker == std::string::npos) ? existing.size() : endAfterMarker + 1;
        existing.replace(beginPos, replaceEnd - beginPos, managed);
    } else {
        existing += managed;
    }

    writeTextFile(path, existing);
}

void setExecutable(const std::filesystem::path& path) {
    if (::chmod(path.c_str(), 0755) != 0) {
        throw std::runtime_error("Failed to set executable bit: " + path.string());
    }
}

Options parseArgs(int argc, char** argv) {
    const auto exe = executablePath();
    const auto homeEnv = std::getenv("HOME");
    const auto homePath = std::filesystem::path(homeEnv ? homeEnv : "");

    const auto findBundlePaths = [&](const std::filesystem::path& exePath) -> BundlePaths {
        const auto exeDir = exePath.parent_path();
        const auto prefixDir = exeDir.parent_path();

        const std::vector<std::filesystem::path> bundleDirs = {
            prefixDir / "configs",
            prefixDir / "share" / "ds",
            "/usr/local/share/ds",
            "/usr/share/ds",
            homePath / ".config" / "ds",
            std::filesystem::current_path() / "configs"
        };

        for (const auto& dir : bundleDirs) {
            const auto configCandidate = dir / "configs.ds";
            const auto themeCandidate = dir / "themes.ds";
            if (std::filesystem::exists(configCandidate) && std::filesystem::exists(themeCandidate)) {
                return BundlePaths{configCandidate, themeCandidate};
            }
        }

        return BundlePaths{prefixDir / "configs" / "configs.ds", prefixDir / "configs" / "themes.ds"};
    };

    const auto discovered = findBundlePaths(exe);
    Options opts{
        .configPath = discovered.configPath,
        .themePath = discovered.themePath,
        .homePath = homePath
    };

    bool configOverridden = false;
    bool themeOverridden = false;

    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto nextValue = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for " + name);
            }
            return argv[++i];
        };

        if (arg == "--configs") {
            opts.configPath = nextValue("--configs");
            configOverridden = true;
        } else if (arg == "--theme") {
            opts.themePath = nextValue("--theme");
            themeOverridden = true;
        } else if (arg == "--home") {
            opts.homePath = nextValue("--home");
        } else if (startsWith(arg, "--")) {
            throw std::runtime_error("Unknown argument: " + arg);
        } else {
            positional.push_back(arg);
        }
    }

    if (!positional.empty()) {
        if (positional[0] == "theme") {
            opts.mode = Options::Mode::EditTheme;
        } else if (positional[0] == "config") {
            opts.mode = Options::Mode::EditConfig;
        } else {
            throw std::runtime_error("Unknown command: " + positional[0]);
        }

        if (positional.size() >= 2) {
            opts.editor = positional[1];
        }
        if (positional.size() > 2) {
            throw std::runtime_error("Too many positional arguments.");
        }
    }

    if (opts.homePath.empty()) {
        throw std::runtime_error("HOME is not set. Use --home to provide a path.");
    }

    if (!configOverridden && !themeOverridden) {
        bootstrapUserBundle(opts);
    }

    return opts;
}

void openInEditor(const std::string& editor, const std::filesystem::path& filePath) {
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("Failed to start editor process.");
    }

    if (pid == 0) {
        std::string fileArg = filePath.string();
        std::vector<char*> args;
        args.push_back(const_cast<char*>(editor.c_str()));
        args.push_back(const_cast<char*>(fileArg.c_str()));
        args.push_back(nullptr);
        execvp(editor.c_str(), args.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        throw std::runtime_error("Failed waiting for editor process.");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("Editor exited with non-zero status.");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto opts = parseArgs(argc, argv);

        if (opts.mode == Options::Mode::EditTheme) {
            openInEditor(opts.editor, opts.themePath);
            return 0;
        }
        if (opts.mode == Options::Mode::EditConfig) {
            openInEditor(opts.editor, opts.configPath);
            return 0;
        }

        const auto theme = parseTheme(opts.themePath);
        const auto blocks = parseConfigs(opts.configPath);

        for (const auto& block : blocks) {
            const auto target = resolveTarget(opts.homePath, block.logicalPath);
            const auto rendered = applyTheme(block.content, theme);

            if (block.logicalPath == ".zshrc") {
                updateZshrc(target, rendered);
            } else {
                writeTextFile(target, rendered);
                if (block.logicalPath == "mango/autostart.sh") {
                    setExecutable(target);
                }
            }
        }

        std::cout << "Successfully updated" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Failed: " << ex.what() << std::endl;
        return 1;
    }
}
