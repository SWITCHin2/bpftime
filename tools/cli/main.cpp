#include "bpftime_shm.hpp"
#include "bpftime_shm_internal.hpp"
#include "bpftime_config.hpp"
#include "bpftime_logger.hpp"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <frida-core.h>
#include <argparse/argparse.hpp>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <vector>
#include <string>
#include <utility>
#include <tuple>
#include <sys/wait.h>

#ifdef __APPLE__
#include <crt_externs.h>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <unistd.h>
inline char **get_environ()
{
	return *_NSGetEnviron();
}
constexpr const char *AGENT_LIBRARY = "libbpftime-agent.dylib";
constexpr const char *SYSCALL_SERVER_LIBRARY =
	"libbpftime-syscall-server.dylib";
constexpr const char *AGENT_TRANSFORMER_LIBRARY =
	"libbpftime-agent-transformer.dylib";
const char *strchrnul(const char *s, int c)
{
	while (*s && *s != (char)c) {
		s++;
	}
	return s;
}
int execvpe(const char *file, char *const argv[], char *const envp[])
{
	for (const char *path = getenv("PATH"); path && *path;
	     path = strchr(path, ':') + 1) {
		char buf[PATH_MAX];
		const char *end = strchrnul(path, ':');
		size_t len = end - path;
		memcpy(buf, path, len);
		buf[len] = '/';
		strcpy(buf + len + 1, file);
		execve(buf, argv, envp);
		if (errno != ENOENT)
			return -1;
	}
	errno = ENOENT;
	return -1;
}
#elif __linux__
extern char **environ;
constexpr const char *AGENT_LIBRARY = "libbpftime-agent.so";
constexpr const char *SYSCALL_SERVER_LIBRARY = "libbpftime-syscall-server.so";
constexpr const char *AGENT_TRANSFORMER_LIBRARY =
	"libbpftime-agent-transformer.so";
#else
#error "Unsupported Platform"
#endif

static int subprocess_pid = 0;

static bool str_starts_with(const char *main, const char *pat)
{
	if (strstr(main, pat) == main)
		return true;
	return false;
}

static int run_command(const char *path, const std::vector<std::string> &argv,
		       const char *ld_preload, const char *agent_so, std::vector<const char *> &env_args)
{
	int pid = fork();
	if (pid == 0) {
		std::string ld_preload_str("LD_PRELOAD=");
		std::string agent_so_str("AGENT_SO=");
		ld_preload_str += ld_preload;

		if (agent_so) {
			agent_so_str += agent_so;
		}
		std::vector<const char *> env_arr;
#if __APPLE__
		char **p = get_environ();
#else
		char **p = environ;
#endif
		while (*p) {
			env_arr.push_back(*p);
			p++;
		}
		bool ld_preload_set = false, agent_so_set = false;
		for (auto &s : env_arr) {
			if (str_starts_with(s, "LD_PRELOAD=")) {
				s = ld_preload_str.c_str();
				ld_preload_set = true;
			} else if (str_starts_with(s, "AGENT_SO=")) {
				s = agent_so_str.c_str();
				agent_so_set = true;
			}
		}
		if (!ld_preload_set)
			env_arr.push_back(ld_preload_str.c_str());
		if (!agent_so_set)
			env_arr.push_back(agent_so_str.c_str());
		if (env_args.size() > 0) 
			env_arr.insert(env_arr.end(), env_args.begin(), env_args.end());
		env_arr.push_back(nullptr);
		std::vector<const char *> argv_arr;
		argv_arr.push_back(path);
		for (const auto &str : argv)
			argv_arr.push_back(str.c_str());
		argv_arr.push_back(nullptr);
		execvpe(path, (char *const *)argv_arr.data(),
			(char *const *)env_arr.data());
	} else {
		subprocess_pid = pid;
		int status;
		if (int cid = waitpid(pid, &status, 0); cid > 0) {
			if (WIFEXITED(status)) {
				int exit_code = WEXITSTATUS(status);
				if (exit_code != 0) {
					spdlog::error(
						"Program exited abnormally, code={}",
						exit_code);
					return 1;
				}
			}
		}
	}
	return 1;
}
static int inject_by_frida(int pid, const char *inject_path, const char *arg)
{
	spdlog::info("Injecting to {}", pid);
	frida_init();
	auto injector = frida_injector_new();
	GError *err = nullptr;
	auto id = frida_injector_inject_library_file_sync(injector, pid,
							  inject_path,
							  "bpftime_agent_main",
							  arg, nullptr, &err);
	if (err) {
		spdlog::error("Failed to inject: {}", err->message);
		g_error_free(err);
		frida_unref(injector);
		frida_deinit();
		return 1;
	}
	spdlog::info("Successfully injected. ID: {}", id);
	frida_injector_close_sync(injector, nullptr, nullptr);
	frida_unref(injector);
	frida_deinit();
	return 0;
}

static std::tuple<std::string, std::vector<std::string>, std::vector<const char *>>
extract_path_and_args(const argparse::ArgumentParser &parser)
{
	std::vector<std::string> items;
	std::vector<const char *> env_args;
	try {
		items = parser.get<std::vector<std::string> >("COMMAND");

		if (parser.is_subcommand_used("load") || parser.is_subcommand_used("start")) {
			if (parser.get<bool>("--no-jit")) {
				const char *disable_jit_str = "BPFTIME_DISABLE_JIT=true";
				env_args.push_back(disable_jit_str);
			}
			if (parser.get<bool>("--run-wit-kernel")) {
				const char *run_with_kernel_verifier = "BPFTIME_RUN_WITH_KERNEL=true";
				env_args.push_back(run_with_kernel_verifier);
			}
			if (parser.is_used("--bpftime-not-load-pattern")) {
				std::string not_load_regex_pattern = parser.get<std::string>("--bpftime-not-load-pattern");
				std::string not_load_pattern_string("BPFTIME_NOT_LOAD_PATTERN=");
				not_load_pattern_string += not_load_regex_pattern; 
				env_args.push_back(not_load_pattern_string.c_str());
			}
			if (parser.is_used("--spdlog-level")) {
				std::string log_level = parser.get<std::string>("--spdlog-level");
				std::string log_level_string("SPDLOG_LEVEL=");
				log_level_string += log_level;
				env_args.push_back(log_level_string.c_str());
			}
			if (parser.is_used("--bpftime-log-path")) {
				std::string log_path = parser.get<std::string>("--bpftime-log-path");
				std::string log_path_string("BPFTIME_LOG_OUTPUT=");
				log_path_string += log_path;
				env_args.push_back(log_path_string.c_str());
			}
			if (parser.get<bool>("--allow-enternal-maps")) {
				const char *allow_external_maps = "BPFTIME_ALLOW_EXTERNAL_MAPS=true";
				env_args.push_back(allow_external_maps);
			}
			if (parser.is_used("--memory_size()")) {
				std::string shm_size_str = std::to_string(parser.get<int>("--memory-size"));
				std::string shm_memory_arg_string("BPFTIME_SHM_MEMORY_MB=");
				shm_memory_arg_string += shm_size_str;
				env_args.push_back(shm_memory_arg_string.c_str());
			}
		}
	} catch (std::logic_error &err) {
		std::cerr << parser;
		exit(1);
	}
	std::string executable = items[0];
	items.erase(items.begin());
	return { executable, items, env_args };
}

static void signal_handler(int sig)
{
	if (subprocess_pid) {
		kill(subprocess_pid, sig);
	}
}

int main(int argc, const char **argv)
{
	const auto agent_config = bpftime::construct_agent_config_from_env();
	bpftime::bpftime_set_logger(agent_config.get_logger_output_path());
	signal(SIGINT, signal_handler);
	signal(SIGTSTP, signal_handler);
	argparse::ArgumentParser program(argv[0]);

	if (auto home_env = getenv("HOME"); home_env) {
		std::string default_location(home_env);
		default_location += "/.bpftime";
		program.add_argument("-i", "--install-location")
			.help("Installing location of bpftime")
			.default_value(default_location)
			.required()
			.nargs(1);
	} else {
		spdlog::warn(
			"Unable to determine home directory. You must specify --install-location");
		program.add_argument("-i", "--install-location")
			.help("Installing location of bpftime")
			.required()
			.nargs(1);
	}

	program.add_argument("-d", "--dry-run")
		.help("Run without commiting any modifications")
		.flag();

	argparse::ArgumentParser load_command("load");

	load_command.add_description(
		"Start an application with bpftime-server injected")
		.add_epilog("For more infomation and options, please see https://eunomia.dev/bpftime");
	
	load_command.add_argument("COMMAND")
		.help("Command to run")
		.nargs(argparse::nargs_pattern::at_least_one)
		.remaining();

	load_command.add_argument("--no-jit")
		.help("Same as BPFTIME_DISABLE_JIT, disable JIT and use intepreter")
		.flag()	;
	load_command.add_argument("--run-with-kernel-verifier")
		.help("Same as BPFTIME_RUN_WITH_KERNEL, load the eBPF application with kernel eBPF loader and kernel verifier.")
		.flag();
	load_command.add_argument("--bpftime-not-load-pattern")
		.help("Same as BPFTIME_NOT_LOAD_PATTERN, regular expression to match the program name to skip loading the eBPF program into the kernel when the BPFTIME_RUN_WITH_KERNEL is set.");
	load_command.add_argument("--spdlog-level")
		.help("Same as SPDLOG_LEVEL, control the log level dynamically, Available log level include:trace, debug, info, warn, err, critical, off");
	load_command.add_argument("--bpftime-log-path")
		.help("Same as BPFTIME_LOG_OUTPUT, control the log output path by setting the BPFTIME_LOG_OUTPUT environment variable.");
	load_command.add_argument("--allow-enternal-maps")
		.help("Same as BPFTIME_ALLOW_EXTERNAL_MAPS, allow external(Unsupport) maps load with the bpftime syscall-server library,")
		.flag();
	load_command.add_argument("--memory-size")
		.help("Same as BPFTIME_SHM_MEMORY_MB, set memory size for shared memory maps.")
		.nargs(1)
    	.scan<'i', int>();

	argparse::ArgumentParser start_command("start");

	start_command.add_description(
		"Start an application with bpftime-agent injected")
		.add_epilog("For more infomation and options, please see https://eunomia.dev/bpftime");;
	start_command.add_argument("-s", "--enable-syscall-trace")
		.help("Whether to enable syscall trace")
		.flag();
	start_command.add_argument("COMMAND")
		.nargs(argparse::nargs_pattern::at_least_one)
		.remaining()
		.help("Command to run");
	
	start_command.add_argument("--no-jit")
		.help("Same as BPFTIME_DISABLE_JIT, disable JIT and use intepreter")
		.flag()	;
	start_command.add_argument("--run-wit-kernel-verifier")
		.help("Same as BPFTIME_RUN_WITH_KERNEL, load the eBPF application with kernel eBPF loader and kernel verifier.")
		.flag();
	start_command.add_argument("--bpftime-not-load-pattern")
		.help("Same as BPFTIME_NOT_LOAD_PATTERN, regular expression to match the program name to skip loading the eBPF program into the kernel when the BPFTIME_RUN_WITH_KERNEL is set.");
	start_command.add_argument("--spdlog-level")
		.help("Same as SPDLOG_LEVEL, control the log level dynamically, Available log level include:trace, debug, info, warn, err, critical, off");
	start_command.add_argument("--bpftime-log-path")
		.help("Same as BPFTIME_LOG_OUTPUT, control the log output path by setting the BPFTIME_LOG_OUTPUT environment variable.");
	start_command.add_argument("--allow-enternal-maps")
		.help("Same as BPFTIME_ALLOW_EXTERNAL_MAPS, allow external(Unsupport) maps load with the bpftime syscall-server library,")
		.flag();
	start_command.add_argument("--memory-size")
		.help("Same as BPFTIME_SHM_MEMORY_MB, set memory size for shared memory maps.")
		.nargs(1)
    	.scan<'i', int>();

	argparse::ArgumentParser attach_command("attach");

	attach_command.add_description("Inject bpftime-agent to a certain pid")
	.add_epilog("For more infomation and options, please see https://eunomia.dev/bpftime");;
	attach_command.add_argument("-s", "--enable-syscall-trace")
		.help("Whether to enable syscall trace")
		.flag();
	attach_command.add_argument("PID").scan<'i', int>();

	argparse::ArgumentParser detach_command("detach");
	detach_command.add_description("Detach all attached agents")
	.add_epilog("For more infomation and options, please see https://eunomia.dev/bpftime");;

	program.add_subparser(load_command);
	program.add_subparser(start_command);
	program.add_subparser(attach_command);
	program.add_subparser(detach_command);
	try {
		program.parse_args(argc, argv);
	} catch (const std::exception &err) {
		std::cerr << err.what() << std::endl;
		std::cerr << program;
		std::exit(1);
	}
	if (!program) {
		std::cerr << program;
		std::exit(1);
	}
	std::filesystem::path install_path(program.get("install-location"));
	if (program.is_subcommand_used("load")) {
		auto so_path = install_path / SYSCALL_SERVER_LIBRARY;
		if (!std::filesystem::exists(so_path)) {
			spdlog::error("Library not found: {}", so_path.c_str());
			return 1;
		}
		auto [executable_path, extra_args, env_args] =
			extract_path_and_args(load_command);
		return run_command(executable_path.c_str(), extra_args,
				   so_path.c_str(), nullptr, env_args);
	} else if (program.is_subcommand_used("start")) {
		auto agent_path = install_path / AGENT_LIBRARY;
		if (!std::filesystem::exists(agent_path)) {
			spdlog::error("Library not found: {}",
				      agent_path.c_str());
			return 1;
		}
		auto [executable_path, extra_args, env_args] =
			extract_path_and_args(start_command);
		if (start_command.get<bool>("enable-syscall-trace")) {
			auto transformer_path =
				install_path /
				"libbpftime-agent-transformer.so";
			if (!std::filesystem::exists(transformer_path)) {
				spdlog::error("Library not found: {}",
					      transformer_path.c_str());
				return 1;
			}

			return run_command(executable_path.c_str(), extra_args,
					   transformer_path.c_str(),
					   agent_path.c_str(), env_args);
		} else {
			return run_command(executable_path.c_str(), extra_args,
					   agent_path.c_str(), nullptr, env_args);
		}
	} else if (program.is_subcommand_used("attach")) {
		auto agent_path = install_path / AGENT_LIBRARY;
		if (!std::filesystem::exists(agent_path)) {
			spdlog::error("Library not found: {}",
				      agent_path.c_str());
			return 1;
		}
		auto pid = attach_command.get<int>("PID");
		if (attach_command.get<bool>("enable-syscall-trace")) {
			auto transformer_path =
				install_path /
				"libbpftime-agent-transformer.so";
			if (!std::filesystem::exists(transformer_path)) {
				spdlog::error("Library not found: {}",
					      transformer_path.c_str());
				return 1;
			}
			return inject_by_frida(pid, transformer_path.c_str(),
					       agent_path.c_str());
		} else {
			return inject_by_frida(pid, agent_path.c_str(), "");
		}
	} else if (program.is_subcommand_used("detach")) {
		SPDLOG_DEBUG("Detaching..");
		try {
			bpftime_initialize_global_shm(
				bpftime::shm_open_type::SHM_OPEN_ONLY);
		} catch (std::exception &ex) {
			SPDLOG_WARN(
				"Shared memory not created, seems syscall server is not running");
			return 0;
		}
		bool sended = false;
		bpftime::shm_holder.global_shared_memory
			.iterate_all_pids_in_alive_agent_set([&](int pid) {
				SPDLOG_INFO("Delivering SIGUSR1 to {}", pid);
				int err = kill(pid, SIGUSR1);
				if (err < 0) {
					SPDLOG_WARN(
						"Unable to signal process {}: {}",
						pid, strerror(errno));
				}
				sended = true;
			});
		if (!sended) {
			SPDLOG_INFO("No process was signaled.");
		}
	}
	return 0;
}
