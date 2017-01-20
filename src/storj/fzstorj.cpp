#include <stdio.h>

#include "events.hpp"

#include <libfilezilla/format.hpp>

#include "storj.h"

template<typename ...Args>
void fzprintf(storjEvent event, Args &&... args)
{
	fputc('0' + static_cast<int>(event), stdout);

	std::string s = fz::sprintf(std::forward<Args>(args)...);
	fwrite(s.c_str(), s.size(), 1, stdout);

	fputc('\n', stdout);
	fflush(stdout);
}

bool getLine(std::string & line)
{
	line.clear();
	while (true) {
		int c = fgetc(stdin);
		if (c == -1) {
			return false;
		}
		else if (!c) {
			return line.empty();
		}
		else if (c == '\n') {
			return !line.empty();
		}
		else if (c == '\r') {
			continue;
		}
		else {
			line += static_cast<unsigned char>(c);
		}
	}
}

static void get_buckets_callback(uv_work_t *work_req, int status)
{
	if (status != 0) {
		fzprintf(storjEvent::Error, "Request failed with outer status code %d", status);
		exit(1);
	}

	json_request_t *req = static_cast<json_request_t *>(work_req->data);

	if (req->status_code != 200) {
		fzprintf(storjEvent::Error, "Request failed with status code %d", req->status_code);
		exit(1);
	}

	if (req->response == NULL) {
		free(req);
		free(work_req);
		fzprintf(storjEvent::Error, "Failed to list buckets for an unknown reason.");
		exit(1);
	}

	int num_buckets = json_object_array_length(req->response);
	struct json_object *bucket{};

	for (int i = 0; i < num_buckets; ++i) {
		bucket = json_object_array_get_idx(req->response, i);

		struct json_object *id_obj{};
		json_object_object_get_ex(bucket, "id", &id_obj);
		struct json_object *name_obj{};
		json_object_object_get_ex(bucket, "name", &name_obj);

		if (id_obj && name_obj) {
			char const* name = json_object_get_string(name_obj);
			char const* id = json_object_get_string(id_obj);
			if (name && *name && id && *id) {
				std::string sname = name;
				std::string sid = id;
				fz::replace_substrings(sname, "\r", "");
				fz::replace_substrings(sid, "\r", "");
				fz::replace_substrings(sname, "\n", "");
				fz::replace_substrings(sid, "\n", "");
				fzprintf(storjEvent::Listentry, "%s\n-1\n%s", sname, sid);
			}
		}
	}

	// Cleanup
	json_object_put(req->response);
	free(req);
	free(work_req);
}

static void list_files_callback(uv_work_t *work_req, int status)
{
	if (status != 0) {
		fzprintf(storjEvent::Error, "Request failed with outer status code %d", status);
		exit(1);
	}

	json_request_t *req = static_cast<json_request_t *>(work_req->data);

	if (req->status_code != 200) {
		fzprintf(storjEvent::Error, "Request failed with status code %d", req->status_code);
		exit(1);
	}

	if (req->response == NULL) {
		free(req);
		free(work_req);
		fzprintf(storjEvent::Error, "Failed to list buckets for an unknown reason.");
		exit(1);
	}

	int num_files = json_object_array_length(req->response);
	struct json_object *file{};

	for (int i = 0; i < num_files; ++i) {
		file = json_object_array_get_idx(req->response, i);

		struct json_object *id_obj{};
		json_object_object_get_ex(file, "id", &id_obj);
		struct json_object *name_obj{};
		json_object_object_get_ex(file, "filename", &name_obj);
		struct json_object *size_obj{};
		json_object_object_get_ex(file, "size", &size_obj);

		if (id_obj && name_obj && size_obj) {
			char const* name = json_object_get_string(name_obj);
			char const* id = json_object_get_string(id_obj);
			char const* size = json_object_get_string(size_obj);
			if (name && *name && id && *id && size && *size) {
				std::string sname = name;
				std::string sid = id;
				std::string ssize = size;
				fz::replace_substrings(sname, "\r", "");
				fz::replace_substrings(sid, "\r", "");
				fz::replace_substrings(sname, "\n", "");
				fz::replace_substrings(sid, "\n", "");
				fz::replace_substrings(ssize, "\n", "");
				fz::replace_substrings(ssize, "\n", "");
				fzprintf(storjEvent::Listentry, "%s\n%s\n%s", sname, ssize, sid);
			}
		}
	}

	// Cleanup
	json_object_put(req->response);
	free(req);
	free(work_req);
}

int main()
{
	fzprintf(storjEvent::Reply, "fzStorj started, protocol_version=%d", FZSTORJ_PROTOCOL_VERSION);

	std::string host;
	unsigned short port = 443;
	std::string user;
	std::string pass;

	storj_env_t *env{};


	auto init_env = [&](){
		if (env) {
			return;
		}
		storj_bridge_options_t options{};
		options.host = host.c_str();
		options.proto = "https";
		options.port = port;
		options.user = user.c_str();
		options.pass = pass.c_str();

		storj_encrypt_options_t encrypt_options{};
		encrypt_options.mnemonic = "";

		storj_http_options_t http_options{};
		http_options.user_agent = "FileZilla";

		static storj_log_options_t log_options{};
		log_options.logger = (storj_logger_fn)printf;
		log_options.level = 4;
		env = storj_init_env(&options, &encrypt_options, &http_options, &log_options);
	};

	int ret = 0;
	while (true) {
		std::string command;
		if (!getLine(command)) {
			ret = 1;
			break;
		}

		if (command.empty()) {
			break;
		}

		std::size_t pos = command.find(' ');
		std::string arg;
		if (pos != std::string::npos) {
			arg = command.substr(pos + 1);
			command = command.substr(0, pos);
		}

		if (command == "host") {
			host = arg;
			std::size_t sep = host.find(':');
			if (sep != std::string::npos) {
				port = fz::to_integral<unsigned short>(host.substr(pos + 1));
				host = host.substr(0, pos);
			}
			fzprintf(storjEvent::Done, "1");
		}
		else if (command == "user") {
			user = arg;
			fzprintf(storjEvent::Done, "1");
		}
		else if (command == "pass") {
			pass = arg;
			fzprintf(storjEvent::Done, "1");
		}
		else if (command == "list-buckets") {
			init_env();
			assert(env);
			storj_bridge_get_buckets(env, 0, get_buckets_callback);
			if (uv_run(env->loop, UV_RUN_DEFAULT)) {
				fzprintf(storjEvent::Error, "uv_run failed.");
				exit(1);
			}
			fzprintf(storjEvent::Done, "1");
		}
		else if (command == "list") {
			init_env();
			assert(env);
			if (arg.empty()) {
				fzprintf(storjEvent::Error, "No bucket given");
				continue;
			}
			storj_bridge_list_files(env, arg.c_str(), 0, list_files_callback);
			if (uv_run(env->loop, UV_RUN_DEFAULT)) {
				fzprintf(storjEvent::Error, "uv_run failed.");
				exit(1);
			}
			fzprintf(storjEvent::Done, "1");
		}
		else {
			fzprintf(storjEvent::Error, "No such command: %s", command);
		}

	}

	if (env) {
		storj_destroy_env(env);
	}

	return ret;
}
