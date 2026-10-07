#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/json_value.h"
#include "assistant_session_store.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <memory>
#include <sys/stat.h>

namespace webcool {
namespace ai {
namespace {

const size_t kMaxImportBytes = 32 * 1024 * 1024;
// SQLite ABI result codes; calls use ACL's already configured SQLite runtime.
const int kSqlOk = 0;
const int kSqlRow = 100;
const int kSqlDone = 101;

bool make_directory(const std::string& path) {
    return ::webcool::ai::file_ops::make_private_directory(path);
}

std::string node_text(acl::json_node* node) {
    return ::webcool::ai::json_value::nullable_text(node);
}

long long node_number(acl::json_node* node) {
    return ::webcool::ai::json_value::number(node);
}

bool node_bool(acl::json_node* node) {
    return ::webcool::ai::json_value::text_boolean(node);
}

acl::json_node* object_child(acl::json_node* node, const char* name) {
    return ::webcool::ai::json_value::object_child(node, name);
}

acl::json_node* array_value(acl::json_node* node) {
    return ::webcool::ai::json_value::array_value(node);
}

bool valid_message(const assistant_message_t& message) {
	return (message.role == "user" || message.role == "assistant")
		&& message.text.size() <= kMaxImportBytes / 2
		&& message.text.find('\0') == std::string::npos
		&& message.format.find('\0') == std::string::npos
		&& message.format.size() <= 128 && message.run_id.size() <= 128
		&& message.run_id.find('\0') == std::string::npos && message.input >= -1
		&& message.output >= -1
		&& message.input <= 1000000000000LL && message.output <= 1000000000000LL
		&& message.duration >= 0 && message.duration <= 1000000000000LL;
}

bool safe_database_file(const std::string& path) {
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide)) return false;
	const DWORD attr = GetFileAttributesW(wide.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
	return !(attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
#else
	struct stat st;
	if (lstat(path.c_str(), &st)) return errno == ENOENT;
	return S_ISREG(st.st_mode) && st.st_nlink == 1 && chmod(path.c_str(), 0600) == 0;
#endif
}

struct database_t {
	std::unique_ptr<acl::db_sqlite> db;
	bool transaction = false;
	~database_t() {
		if (transaction && db) db->sqlite3_exec("ROLLBACK", NULL, NULL, NULL);
	}

	bool exec(const char* sql, std::string& err) {
		char* detail = NULL;
		const int rc = db->sqlite3_exec(sql, NULL, NULL, &detail);
		if (rc != kSqlOk) err = detail ? detail : "assistant SQLite operation failed";
		if (detail) db->sqlite3_free(detail);
		return rc == kSqlOk;
	}

	bool open(const std::string& root, std::string& err) {
		const std::string dir = root + "/.webcool_agent";
		if (!make_directory(dir)) {
			err = "cannot create assistant database directory";
			return false;
		}
#ifndef _WIN32
		if (chmod(dir.c_str(), 0700)) {
			err = "cannot protect assistant database directory";
			return false;
		}
#endif
		const std::string path = dir + "/assistant.sqlite3";
		for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
			if (!safe_database_file(path + suffix)) {
				err = "unsafe assistant database path";
				return false;
			}
		}
		db.reset(new acl::db_sqlite(path.c_str(), "utf-8"));
		if (!db->open()) {
			err = db->get_error();
			return false;
		}
		db->set_busy_timeout(5000);
		if (!safe_database_file(path)) {
			err = "cannot protect assistant database";
			return false;
		}
		return exec("PRAGMA foreign_keys=ON", err)
			&& exec("PRAGMA journal_mode=WAL", err)
			&& exec("PRAGMA synchronous=FULL", err)
			&& exec("CREATE TABLE IF NOT EXISTS assistant_sessions ("
				"id TEXT PRIMARY KEY, title TEXT NOT NULL, "
				"updated_at INTEGER NOT NULL)", err)
			&& exec("CREATE TABLE IF NOT EXISTS assistant_messages ("
				"id INTEGER PRIMARY KEY AUTOINCREMENT, "
				"session_id TEXT NOT NULL REFERENCES assistant_sessions(id) "
				"ON DELETE CASCADE, "
				"role TEXT NOT NULL CHECK(role IN ('user','assistant')), "
				"text TEXT NOT NULL, format TEXT NOT NULL DEFAULT '', "
				"run_id TEXT NOT NULL DEFAULT '', "
				"failed INTEGER NOT NULL DEFAULT 0, "
				"input_tokens INTEGER NOT NULL DEFAULT -1, "
				"output_tokens INTEGER NOT NULL DEFAULT -1, "
				"duration_ms INTEGER NOT NULL DEFAULT 0, "
				"created_at INTEGER NOT NULL)", err)
			&& exec("CREATE INDEX IF NOT EXISTS assistant_messages_session "
				"ON assistant_messages(session_id,id)", err)
			&& exec("CREATE UNIQUE INDEX IF NOT EXISTS assistant_messages_run "
				"ON assistant_messages(session_id,run_id,role) "
				"WHERE run_id<>''", err)
			&& exec("CREATE INDEX IF NOT EXISTS assistant_sessions_updated "
				"ON assistant_sessions(updated_at DESC)", err)
			&& exec("CREATE TABLE IF NOT EXISTS assistant_session_pins ("
				"session_id TEXT PRIMARY KEY REFERENCES assistant_sessions(id) "
				"ON DELETE CASCADE)", err)
			&& exec("CREATE TABLE IF NOT EXISTS assistant_session_tags ("
				"session_id TEXT NOT NULL REFERENCES assistant_sessions(id) "
				"ON DELETE CASCADE, tag_id TEXT NOT NULL, "
				"PRIMARY KEY(session_id,tag_id))", err)
			&& exec("CREATE INDEX IF NOT EXISTS assistant_session_tags_tag "
				"ON assistant_session_tags(tag_id)", err)
			&& exec("CREATE TABLE IF NOT EXISTS assistant_images ("
				"id TEXT PRIMARY KEY, session_id TEXT NOT NULL "
				"REFERENCES assistant_sessions(id) ON DELETE CASCADE, "
				"mime TEXT NOT NULL, data BLOB NOT NULL)", err);
	}

	bool begin(std::string& err) {
		transaction = exec("BEGIN IMMEDIATE", err);
		return transaction;
	}

	bool commit(std::string& err) {
		if (!exec("COMMIT", err)) return false;
		transaction = false;
		return true;
	}
};

// All content is bound with explicit lengths, never interpolated into SQL.
struct statement_t {
	acl::db_sqlite& db;
	sqlite3_stmt* stmt = NULL;
	bool bound = true;
	explicit statement_t(database_t& connection) : db(*connection.db) {
	}

	~statement_t() {
		if (stmt) db.sqlite3_finalize(stmt);
	}

	bool prepare(const char* sql, std::string& err) {
		if (db.sqlite3_prepare_v2(sql, -1, &stmt, NULL) != kSqlOk) {
			err = "cannot prepare assistant SQLite statement";
			return false;
		}
		return true;
	}

	void text(int n, const std::string& value) {
		// SQLITE_STATIC: caller's values remain alive until this statement is finalized.
		bound = bound && db.sqlite3_bind_text(stmt, n, value.data(),
			static_cast<int>(value.size()), NULL) == kSqlOk;
	}

	void number(int n, long long value) {
		bound = bound && db.sqlite3_bind_int64(stmt, n, value) == kSqlOk;
	}

	int step(std::string& err) {
		const int rc = bound ? db.sqlite3_step(stmt) : -1;
		if (rc != kSqlRow && rc != kSqlDone) err = "assistant SQLite read/write failed";
		return rc;
	}

	std::string text(int n) {
		const unsigned char* value = db.sqlite3_column_text(stmt, n);
		return value ? std::string(reinterpret_cast<const char*>(value),
			db.sqlite3_column_bytes(stmt, n)) : "";
	}

	long long number(int n) {
		return db.sqlite3_column_int64(stmt, n);
	}
};

bool read_session(database_t& db, const std::string& id,
	assistant_session_t& session, bool& found, std::string& err)
{
	session = assistant_session_t();
	found = false;
	statement_t meta(db);
	if (!meta.prepare("SELECT title,updated_at,EXISTS(SELECT 1 "
		"FROM assistant_session_pins WHERE session_id=id) "
		"FROM assistant_sessions WHERE id=?",
		err))
	{
		return false;
	}
	meta.text(1, id);
	int rc = meta.step(err);
	if (rc == kSqlDone) return true;
	if (rc != kSqlRow) return false;
	session.id = id;
	session.title = meta.text(0);
	session.updated = meta.number(1);
	session.pinned = meta.number(2) != 0;
	found = true;
	statement_t rows(db);
	if (!rows.prepare("SELECT role,text,format,run_id,failed,input_tokens,"
		"output_tokens,duration_ms,created_at FROM assistant_messages "
		"WHERE session_id=? ORDER BY id", err))
	{
		return false;
	}
	rows.text(1, id);
	while ((rc = rows.step(err)) == kSqlRow) {
		assistant_message_t message;
		message.role = rows.text(0);
		message.text = rows.text(1);
		message.format = rows.text(2);
		message.run_id = rows.text(3);
		message.failed = rows.number(4) != 0;
		message.input = rows.number(5);
		message.output = rows.number(6);
		message.duration = rows.number(7);
		message.created_at = rows.number(8);
		session.messages.push_back(message);
	}
	if (rc != kSqlDone) return false;
	statement_t tags(db);
	if (!tags.prepare("SELECT tag_id FROM assistant_session_tags "
		"WHERE session_id=? ORDER BY tag_id", err))
	{
		return false;
	}
	tags.text(1, id);
	while ((rc = tags.step(err)) == kSqlRow) session.tag_ids.push_back(tags.text(0));
	return rc == kSqlDone;
}

bool insert_message(database_t& db, const std::string& id,
	const assistant_message_t& message, long long now, std::string& err)
{
	statement_t row(db);
	if (!row.prepare("INSERT INTO assistant_messages("
		"session_id,role,text,format,run_id,failed,input_tokens,"
		"output_tokens,duration_ms,created_at) VALUES(?,?,?,?,?,?,?,?,?,?)", err))
	{
		return false;
	}
	row.text(1, id);
	row.text(2, message.role);
	row.text(3, message.text);
	row.text(4, message.format);
	row.text(5, message.run_id);
	row.number(6, message.failed ? 1 : 0);
	row.number(7, message.input);
	row.number(8, message.output);
	row.number(9, message.duration);
	row.number(10, now);
	return row.step(err) == kSqlDone;
}

bool insert_session(database_t& db, const std::string& id, const std::string& title,
	long long now, std::string& err)
{
	statement_t row(db);
	if (!row.prepare("INSERT INTO assistant_sessions(id,title,updated_at) "
		"VALUES(?,?,?)", err))
	{
		return false;
	}
	row.text(1, id);
	row.text(2, title);
	row.number(3, now);
	return row.step(err) == kSqlDone;
}

} // namespace

bool assistant_session_id_valid(const std::string& id) {
	if (id.empty() || id.size() > 128) return false;
	for (char c : id) {
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '-' || c == '_'))
		{
			return false;
		}
	}
	return true;
}

bool assistant_session_parse(acl::json_node* node, assistant_session_t& session,
	std::string& err)
{
	session = assistant_session_t();
	session.id = node_text(object_child(node, "id"));
	session.title = node_text(object_child(node, "title"));
	session.updated = node_number(object_child(node, "updated_at"));
	acl::json_node* messages = array_value(object_child(node, "messages"));
	if (!assistant_session_id_valid(session.id) || session.title.size() > 1024
		|| session.title.find('\0') != std::string::npos || !messages)
	{
		err = "invalid assistant conversation";
		return false;
	}
	size_t bytes = 0;
	for (acl::json_node* item = messages->first_child(); item != NULL;
		item = messages->next_child())
	{
		assistant_message_t message;
		message.role = node_text(object_child(item, "role"));
		message.text = node_text(object_child(item, "text"));
		message.format = node_text(object_child(item, "format"));
		message.failed = node_bool(object_child(item, "failed"));
		message.run_id = node_text(object_child(item, "run_id"));
		acl::json_node* metrics = object_child(item, "metrics");
		acl::json_node* input = object_child(metrics, "input");
		acl::json_node* output = object_child(metrics, "output");
		message.input = input && input->get_int64() ? node_number(input) : -1;
		message.output = output && output->get_int64() ? node_number(output) : -1;
		message.duration = std::max(0LL, node_number(object_child(metrics,
			"durationMs")));
		bytes += message.text.size();
		if (!valid_message(message) || message.format.size() > 128
			|| message.run_id.size() > 128 || bytes > kMaxImportBytes / 2
			|| session.messages.size() >= 10000)
		{
			err = "invalid or oversized assistant messages";
			return false;
		}
		session.messages.push_back(message);
	}
	return true;
}

void assistant_session_json(acl::json& json, acl::json_node& node,
	const assistant_session_t& session, bool detail)
{
	node.add_text("id", session.id.c_str());
	node.add_text("title", session.title.c_str());
	node.add_number("updated_at", session.updated);
	node.add_bool("pinned", session.pinned);
	if (!detail) return;
	acl::json_node& tags = json.create_array();
	node.add_child("tag_ids", tags);
	for (const std::string& tag_id : session.tag_ids) {
		tags.add_child(json.create_array_text(tag_id.c_str()));
	}
	node.add_number("message_count", static_cast<long long>(session.messages.size()));
	acl::json_node& messages = json.create_array();
	node.add_child("messages", messages);
	for (const assistant_message_t& message : session.messages) {
		acl::json_node& item = messages.add_child(false, true);
		item.add_text("role", message.role.c_str());
		item.add_text("text", message.text.c_str());
		item.add_text("format", message.format.c_str());
		item.add_text("run_id", message.run_id.c_str());
		item.add_bool("failed", message.failed);
		item.add_number("created_at", message.created_at);
		if (message.role == "assistant") {
			acl::json_node& metrics = json.create_node();
			item.add_child("metrics", metrics);
			if (message.input >= 0 && message.output >= 0) {
				metrics.add_number("input", message.input);
				metrics.add_number("output", message.output);
				metrics.add_number("total", message.input + message.output);
			}
			metrics.add_number("durationMs", message.duration);
		}
	}
}

assistant_session_store_t::assistant_session_store_t(const std::string& user_root)
	: user_root_(user_root)
{
}

bool assistant_session_store_t::get(const std::string& id,
	assistant_session_t& session, std::string& err) const
{
	if (!assistant_session_id_valid(id)) {
		err = "invalid assistant conversation id";
		return false;
	}
	database_t db;
	bool found;
	if (!db.open(user_root_, err) || !read_session(db, id, session, found, err)) {
		return false;
	}
	if (!found) err = "assistant conversation not found";
	return found;
}

bool assistant_session_store_t::list(std::vector<assistant_session_t>& sessions,
	std::string& err) const
{
	database_t db;
	if (!db.open(user_root_, err)) return false;
	statement_t rows(db);
	if (!rows.prepare("SELECT id,title,updated_at,EXISTS(SELECT 1 "
		"FROM assistant_session_pins WHERE session_id=id) AS pinned "
		"FROM assistant_sessions ORDER BY pinned DESC,updated_at DESC,id", err))
	{
		return false;
	}
	sessions.clear();
	int rc;
	while ((rc = rows.step(err)) == kSqlRow) {
		assistant_session_t session;
		session.id = rows.text(0);
		session.title = rows.text(1);
		session.updated = rows.number(2);
		session.pinned = rows.number(3) != 0;
		sessions.push_back(session);
	}
	return rc == kSqlDone;
}

bool assistant_session_store_t::import_session(const assistant_session_t& session,
	std::string& err) const
{
	if (!assistant_session_id_valid(session.id)) {
		err = "invalid assistant conversation id";
		return false;
	}
	database_t db;
	assistant_session_t existing;
	bool found;
	if (!db.open(user_root_, err) || !db.begin(err) || !read_session(db, session.id,
		existing, found, err))
	{
		return false;
	}
	if (found) {
		if (existing.messages.size() < session.messages.size()) {
			err = "assistant migration conflicts with server history";
			return false;
		}
		for (size_t i = 0; i < session.messages.size(); ++i) {
			if (existing.messages[i].role != session.messages[i].role
				|| existing.messages[i].text != session.messages[i].text)
			{
				err = "assistant migration conflicts with server history";
				return false;
			}
		}
		return db.commit(err);
	}
	const long long now = static_cast<long long>(time(NULL)) * 1000;
	if (!insert_session(db, session.id, session.title, now, err)) return false;
	for (const assistant_message_t& message : session.messages) {
		if (!valid_message(message)) {
			err = "invalid assistant message";
			return false;
		}
		if (!insert_message(db, session.id, message, now, err)) return false;
	}
	return db.commit(err);
}

bool assistant_session_store_t::append(const std::string& id,
	const assistant_message_t& message, std::string& err) const
{
	if (!assistant_session_id_valid(id) || !valid_message(message)
		|| message.run_id.empty())
	{
		err = "invalid assistant message";
		return false;
	}
	database_t db;
	if (!db.open(user_root_, err) || !db.begin(err)) return false;
	const long long now = static_cast<long long>(time(NULL)) * 1000;
	{
		statement_t meta(db);
		if (!meta.prepare("SELECT id FROM assistant_sessions WHERE id=?", err)) {
			return false;
		}
		meta.text(1, id);
		const int rc = meta.step(err);
		if (rc == kSqlDone) {
			if (message.role != "user") {
				err = "assistant conversation not found";
				return false;
			}
			size_t length = std::min(size_t(108), message.text.size());
			while (length < message.text.size() && length
				&& (static_cast<unsigned char>(message.text[length]) & 0xc0) == 0x80)
			{
				--length;
			}
			if (!insert_session(db, id, message.text.substr(0, length), now, err)) {
				return false;
			}
		} else if (rc != kSqlRow) return false;
	}
	{
		statement_t duplicate(db);
		if (!duplicate.prepare("SELECT id FROM assistant_messages "
			"WHERE session_id=? AND run_id=? AND role=?", err))
		{
			return false;
		}
		duplicate.text(1, id);
		duplicate.text(2, message.run_id);
		duplicate.text(3, message.role);
		const int rc = duplicate.step(err);
		if (rc == kSqlRow) return db.commit(err);
		if (rc != kSqlDone) return false;
	}
	if (!insert_message(db, id, message, now, err)) return false;
	{
		statement_t update(db);
		if (!update.prepare("UPDATE assistant_sessions SET updated_at=? WHERE id=?",
			err))
		{
			return false;
		}
		update.number(1, now);
		update.text(2, id);
		if (update.step(err) != kSqlDone) return false;
	}
	return db.commit(err);
}

bool assistant_session_store_t::update(const std::string& operation,
	const std::vector<std::string>& ids, const std::string& title,
	std::string& err) const
{
	if ((operation != "rename" && operation != "pin"
		&& operation != "unpin" && operation != "delete")
		|| ids.empty() || ids.size() > 1000)
	{
		err = "invalid assistant conversation operation";
		return false;
	}
	if (operation == "rename" && (ids.size() != 1 || title.empty()
		|| title.size() > 1024 || title.find('\0') != std::string::npos))
	{
		err = "invalid assistant conversation title";
		return false;
	}
	database_t db;
	if (!db.open(user_root_, err) || !db.begin(err)) return false;
	// Validate the entire batch before changing any record.
	for (const std::string& id : ids) {
		if (!assistant_session_id_valid(id)) {
			err = "invalid assistant conversation id";
			return false;
		}
		statement_t check(db);
		if (!check.prepare("SELECT id FROM assistant_sessions WHERE id=?", err)) {
			return false;
		}
		check.text(1, id);
		const int rc = check.step(err);
		if (rc != kSqlRow) {
			if (rc == kSqlDone) err = "assistant conversation not found";
			return false;
		}
	}
	for (const std::string& id : ids) {
		statement_t change(db);
		const char* sql = operation == "rename"
			? "UPDATE assistant_sessions SET title=? WHERE id=?"
			: operation == "pin"
			? "INSERT OR IGNORE INTO assistant_session_pins(session_id) VALUES(?)"
			: operation == "unpin"
			? "DELETE FROM assistant_session_pins WHERE session_id=?"
			: "DELETE FROM assistant_sessions WHERE id=?";
		if (!change.prepare(sql, err)) return false;
		if (operation == "rename") {
			change.text(1, title);
			change.text(2, id);
		} else {
			change.text(1, id);
		}
		if (change.step(err) != kSqlDone) return false;
	}
	return db.commit(err);
}

bool assistant_session_store_t::save_image(const std::string& id,
	const std::string& session_id, const std::string& mime,
	const std::string& bytes, std::string& err) const
{
	if (!assistant_session_id_valid(id) || !assistant_session_id_valid(session_id)
		|| bytes.empty() || bytes.size() > 16 * 1024 * 1024
		|| (mime != "image/png" && mime != "image/jpeg" && mime != "image/webp"))
	{
		err = "invalid generated image";
		return false;
	}
	database_t db;
	if (!db.open(user_root_, err)) return false;
	statement_t insert(db);
	if (!insert.prepare("INSERT INTO assistant_images(id,session_id,mime,data) "
		"VALUES(?,?,?,?)", err)) return false;
	insert.text(1, id);
	insert.text(2, session_id);
	insert.text(3, mime);
	insert.bound = insert.bound && insert.db.sqlite3_bind_blob(insert.stmt, 4,
		bytes.data(), static_cast<int>(bytes.size()), NULL) == kSqlOk;
	return insert.step(err) == kSqlDone;
}

bool assistant_session_store_t::load_image(const std::string& id,
	std::string& mime, std::string& bytes, std::string& err) const
{
	if (!assistant_session_id_valid(id)) {
		err = "invalid generated image id";
		return false;
	}
	database_t db;
	if (!db.open(user_root_, err)) return false;
	statement_t row(db);
	if (!row.prepare("SELECT mime,data FROM assistant_images WHERE id=?", err)) return false;
	row.text(1, id);
	if (row.step(err) != kSqlRow) {
		if (err.empty()) err = "generated image not found";
		return false;
	}
	mime = row.text(0);
	const void* data = row.db.sqlite3_column_blob(row.stmt, 1);
	const int size = row.db.sqlite3_column_bytes(row.stmt, 1);
	if (!data || size <= 0 || size > 16 * 1024 * 1024) {
		err = "invalid stored image";
		return false;
	}
	bytes.assign(static_cast<const char*>(data), size);
	return true;
}

bool assistant_session_store_t::add_tag(const std::string& id,
	const std::string& tag_id, std::string& err) const
{
	if (!assistant_session_id_valid(id) || tag_id.empty() || tag_id.size() > 128
		|| tag_id.find('\0') != std::string::npos)
	{
		err = "invalid assistant conversation tag";
		return false;
	}
	database_t db;
	if (!db.open(user_root_, err) || !db.begin(err)) return false;
	statement_t insert(db);
	if (!insert.prepare("INSERT OR IGNORE INTO assistant_session_tags "
		"(session_id,tag_id) VALUES(?,?)", err))
	{
		return false;
	}
	insert.text(1, id);
	insert.text(2, tag_id);
	return insert.step(err) == kSqlDone && db.commit(err);
}

bool assistant_session_store_t::list_tag(const std::string& tag_id,
	std::vector<assistant_session_t>& sessions, std::string& err) const
{
	database_t db;
	if (!db.open(user_root_, err)) return false;
	statement_t rows(db);
	if (!rows.prepare("SELECT s.id,s.title,s.updated_at FROM assistant_sessions s "
		"JOIN assistant_session_tags t ON t.session_id=s.id "
		"WHERE t.tag_id=? ORDER BY s.updated_at DESC,s.id", err))
	{
		return false;
	}
	rows.text(1, tag_id);
	sessions.clear();
	int rc;
	while ((rc = rows.step(err)) == kSqlRow) {
		assistant_session_t session;
		session.id = rows.text(0);
		session.title = rows.text(1);
		session.updated = rows.number(2);
		sessions.push_back(session);
	}
	return rc == kSqlDone;
}

bool assistant_session_store_t::remove_tags(const std::vector<std::string>& tag_ids,
	std::string& err) const
{
	database_t db;
	if (!db.open(user_root_, err) || !db.begin(err)) return false;
	for (const std::string& tag_id : tag_ids) {
		statement_t remove(db);
		if (!remove.prepare("DELETE FROM assistant_session_tags WHERE tag_id=?", err)) {
			return false;
		}
		remove.text(1, tag_id);
		if (remove.step(err) != kSqlDone) return false;
	}
	return db.commit(err);
}

} // namespace ai
} // namespace webcool
