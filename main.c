
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>

#include <json-c/json.h>
#include <termbox.h>
#include <sqlite3.h>

#include "mongoose.h"
#include "simclist.h"

// For some reason this isn't part of the stdlib
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

// run in memory for faster resp, but no persistence
#define DB_PATH "slack.db"
// #define DB_PATH ":memory:"

// Formatting
#define CHANS_WIDTH 20
#define USER_WIDTH 10

// Theme colours 
#define STATUSLINE_FG 232
#define STATUSLINE_BG 255
#define TEXTBOX_FG 232
#define TEXTBOX_BG 255
#define DEBUG_FG 22
#define DEBUG_BG 255
#define CHANNELS_FG 254
#define CHANNELS_BG 53
#define CHANNELS_FG_SELECTED 254
#define CHANNELS_BG_SELECTED 54
#define USER_FG 232
#define USER_BG 255
#define MESSAGE_FG 232
#define MESSAGE_FG_UNACKED 245
#define MESSAGE_BG 255
#define MESSAGE_BG_ALT 254

/*
 * Errors are written to this file, rather than a stdin/out since those 
 * are handled by termbox
 */
FILE* errfile;

/*
 * Debug logging is written to this file
 */
FILE* dbgfile;

/*
 * Exits the program with an error message if the error code isn't OK
 */
#define sqlite_check(db, function_call) {\
	int error_code = function_call; \
	if (error_code != SQLITE_OK) { \
		fprintf(errfile, "sqlite3 error at: %s:%d\n" \
		   "%s", __FILE__, __LINE__, sqlite3_errmsg(db)); \
		raise(SIGTERM); \
	} \
}

/*
 * Same as above, but accepts an alternative expected state
 */
#define sqlite_check_ex(db, function_call, expected) {\
	int error_code = function_call; \
	if (error_code != expected) { \
		fprintf(errfile, "sqlite3 error at: %s:%d\n" \
		   "%s", __FILE__, __LINE__, sqlite3_errmsg(db)); \
		raise(SIGTERM); \
	} \
}

// Set to true to terminate the main loop gracefully
bool quit;

// notification of application state change
struct state_update {
	// SQLITE_INSERT, SQLITE_UPDATE, SQLITE_DELETE
	int operation;
	const char* database;
	const char* tablename;
	sqlite3_int64 rowid;
};

// this function will take it's  own copy of 
// database and tablename
struct state_update* database_update(const char* database,
		int operation,
		const char* tablename,
		int rowid) {
	struct state_update* su = malloc(sizeof(struct state_update));
	su->database = strdup(database);
	su->operation = operation;
	su->tablename = strdup(tablename);
	su->rowid = rowid;
	return su;
}

void free_state_update(struct state_update* u) {
	free((void*)u->database);
	free((void*)u->tablename);
	free(u);
}

// When application state is updated,
// add notification to this queue of type state_update
list_t state_update_queue;

// Listeners for application state changes
list_t state_listeners;

// All slack data and UI state is stored in sqlite 
sqlite3* db;

// UI state
enum mode {
	mode_normal = 0,
	mode_insert = 1,
	mode_search = 2
};

// Networking stuff
static const char* slack_rtm_connect_url = "https://slack.com/api/rtm.connect";
static const char* slack_conversations_list_url = "https://slack.com/api/conversations.list?types=public_channel,private_channel,mpim,im&limit=1000&exclude_archived=true";
static const char* slack_users_list_url = "https://slack.com/api/users.list";
static const char* slack_conversation_history_url = "https://slack.com/api/conversations.history?channel=%s";
struct mg_mgr mgr;
struct mg_connection* ws_connection;

// For debug logging
void dbg(const char* format, ...) { 
	va_list args;
	va_start (args, format);
	va_end (args);
	vfprintf(dbgfile, format, args);
	fprintf(dbgfile, "\n");
	fflush(dbgfile);
}

// Caller responsible for freeing
char* format_url1(const char* format, const char* p1) { 
	int max = strlen(format) + strlen(p1);
	char* res = malloc(max);
	snprintf(res, max, format, p1);
	return res;
}

void set_key_value_int(const char* key, int value) {
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"insert into kvs (key, value) "
				"values (?, ?) "
				"on conflict (key) "
				"do update set value=excluded.value", -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 1, key, -1, NULL));
	sqlite_check(db, sqlite3_bind_int(stmt, 2, value));
	sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
	sqlite3_finalize(stmt);
}

int get_key_value_int(const char* key, int default_val) {
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"select value "
				"from kvs "
				"where key = ?"
				, -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 1, key, -1, NULL));
	int res;
	int v = sqlite3_step(stmt);
	if (v == SQLITE_ROW) {
		res = sqlite3_column_int(stmt, 0);
	} else if (v == SQLITE_DONE) {
		res = default_val;
	} else {
		sqlite_check(db, v);
	}
	return res;
}
char* get_key_value_key_by_rowid(int rowid) {
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"select key "
				"from kvs "
				"where rowid = ?"
				, -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_int(stmt, 1, rowid));
	char* res;
	int v = sqlite3_step(stmt);
	if (v == SQLITE_ROW) {
		res = strdup(sqlite3_column_text(stmt, 0));
	} else if (v == SQLITE_DONE) {
		res = NULL;
	} else {
		sqlite_check(db, v);
	}
	return res;
}
void set_key_value_string(const char* key, char* value) {
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"insert into kvs (key, value) "
				"values (?, ?) "
				"on conflict (key) "
				"do update set value=excluded.value", -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 1, key, -1, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 2, value, -1, NULL));
	sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
	sqlite3_finalize(stmt);
}

// Caller frees
char* get_key_value_string(const char* key, char* default_value) {
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"select value "
				"from kvs "
				"where key = ?"
				, -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 1, key, -1, NULL));
	char* res;
	int v = sqlite3_step(stmt);
	if (v == SQLITE_ROW) {
		res = strdup(sqlite3_column_text(stmt, 0));
	} else if (v == SQLITE_DONE) {
		res = strdup(default_value);
	} else {
		sqlite_check(db, v);
	}
	return res;
}
void set_current_mode(int m) {
	set_key_value_int("mode", m);
}
int get_current_mode() {
	return get_key_value_int("mode", 0);
}

void set_current_user_id(char* u) {
	set_key_value_string("current_user_id", u);
}
char* get_current_user_id() {
	return get_key_value_string("current_user_id", NULL);
}

int count_conversations() {
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, "select count(1) from conversation where is_member = 1", -1, &stmt, NULL));
	sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_ROW);
	int count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

void set_conversation_selection_pos(int new_selection) {
	int max = count_conversations();
	// don't run off the start of the list
	if (new_selection < 0) {
		new_selection = 0;
	}
	// don't run off the end of the list
	if (new_selection >= max) {
		new_selection = max-1;
	}
	set_key_value_int("conversation_selection_pos", new_selection);
}
int get_conversation_selection_pos() {
	return get_key_value_int("conversation_selection_pos", 0);
}

/*
 * Update the window start position for the conversation list
 */
void set_conversation_window_start(int new_window_start) {
	// Don't run off the start of the list
	if (new_window_start < 0) {
		return;
	}
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, "", -1, &stmt, NULL));
	set_key_value_int("conversation_window_start", new_window_start);
}
int get_conversation_window_start() {
	return get_key_value_int("conversation_window_start", 0);
}

const char* mode_desc() {
	switch (get_current_mode()) {
		case mode_normal: return "normal";
		case mode_insert: return "insert";
		case mode_search: return "search";
		default: return "none";
	}
}

// caller should free the returned string
const char* get_selected_conversation_id() {
	int sp = get_conversation_selection_pos();
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, "select id "
				"from conversation "
				"where is_member = 1 "
				"order by name "
				"limit 1 "
				"offset ? "
				, -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_int(stmt, 1, sp));
	int v = sqlite3_step(stmt);
	char* res = NULL;
	if (v  == SQLITE_ROW) {
		res = strdup(sqlite3_column_text(stmt, 0));
	} else if (v  != SQLITE_DONE) {
		sqlite_check(db, v);
	}
	sqlite3_finalize(stmt);
	return res;
}

bool get_conversation_did_fetch(const char* id) {
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"select did_fetch "
				"from conversation "
				"where id = ? "
				, -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 1, id, -1, NULL));
	sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_ROW);
	int r = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return r;
}

void set_conversation_did_fetch(const char* id, bool did_fetch) {
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"update conversation "
				"set did_fetch = ? "
				"where id = ? "
				, -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_int(stmt, 1, did_fetch));
	sqlite_check(db, sqlite3_bind_text(stmt, 2, id, -1, NULL));
	sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
	sqlite3_finalize(stmt);
}

// function pointer indicates size of elements in list
list_t* new_copying_list(size_t (*meter)(const void*)) {
	list_t* res = malloc(sizeof(list_t));
	list_init(res);
	list_attributes_copy(res, meter, true);
	return res;
}

void list_of_lists_destroy(list_t* list) {
	int size = list_size(list);
	for (int i=0; i<size; i++) {
		list_destroy((list_t*)list_get_at(list, i));
	}
	list_destroy(list);
}

size_t u_int32_size(const void* data) {
	return sizeof(u_int32_t);
}

size_t list_t_size(const void* data) {
	return sizeof(list_t);
}

list_t* to_utf8_list(const char* str) {
	list_t* res = new_copying_list(u_int32_size);
	int i=0;
	u_int32_t u;
	while (str[i] != '\0') {
		i += tb_utf8_char_to_unicode(&u, &str[i]);
		list_append(res, &u);
	}
	return res;
}

char* to_char_array(list_t* lst) {
	char buf[1000];
	int size = list_size(lst);
	int i;
	for (i=0; i<size; ) {
		u_int32_t* ch = list_get_at(lst, i);
		i += tb_utf8_unicode_to_char(&buf[i], *ch);
	}
	buf[i] = '\0';
	return strdup(buf);
}

void set_input_buffer(list_t* lst) {
	char* ib = to_char_array(lst);
	set_key_value_string("input_buffer", ib);
	free(ib);
}
list_t* get_input_buffer() {
	char* str = get_key_value_string("input_buffer", "");
	list_t* ret = to_utf8_list(str);
	free(str);
	return ret;
}

void set_input_cursor_pos(int n) {
	list_t* input_buffer = get_input_buffer();
	int max = list_size(input_buffer);
	list_destroy(input_buffer);
	if (n < 0) {
		return;
	} else if (n > max) {
		return;
	}
	set_key_value_int("input_cursor_pos", n);
}
int get_input_cursor_pos() {
	return get_key_value_int("input_cursor_pos", 0);
}

/*
 * Shamelessly taken from stackoverflow https://stackoverflow.com/questions/22582989/word-wrap-program-c
 * I think this is a very dumb wordwrap that won't cope with words longer than the line. 
 * But it's simple at least.
 */
int wordlen(list_t* str, int start){
	int str_len = list_size(str);
	int i=start;
	for (; i<str_len; i++) {
		u_int32_t* u = list_get_at(str, i);
		if (*u == ' ' || *u == '\n') {
			break;
		}
		i++;
	}
	return i-start;
}

// returns a list of lists
list_t* wrap(list_t* input, const int wrapline){
	list_t* result = new_copying_list(list_t_size);
	list_t* line = new_copying_list(u_int32_size);
	int input_len = list_size(input);
	for (int i=0; i<input_len;) {
		u_int32_t* ch = list_get_at(input, i);
		int line_len = list_size(line);
		if (*ch == '\n') {
			list_append(result, line);
			line = new_copying_list(u_int32_size);
			i++;
		} else if (*ch == ' ') {
			// break nicely on spaces
			if (line_len + wordlen(input, i+1) >= wrapline) {
				list_append(result, line);
				line = new_copying_list(u_int32_size);
			} else {
				list_append(line, ch);
			}
			i++;
		} else if (line_len >= wrapline) {
			// forcibly break overly long words
			list_append(result, line);
			line = new_copying_list(u_int32_size);
			// don't increment i!
		} else {
			list_append(line, ch);
			i++;
		}
	}
	list_append(result, line);
	return result;
}

void render_char(u_int32_t ch, int x, int y, int fg, int bg) {
	struct tb_cell c = {
		.ch = ch,
		.fg = fg,
		.bg = bg,
	};
	tb_put_cell(x, y, &c);
}

void render() {
	tb_clear();
	int width = tb_width();
	int height = tb_height();
	
	int bottom_pos = 1;

	// Write the input buffer
	list_t* input_buffer = get_input_buffer();
	int input_buffer_len = list_size(input_buffer);
	for (int i=0; i<MIN(width, input_buffer_len); i++) {
		u_int32_t* ch = list_get_at(input_buffer, i);
		render_char(*ch, i, height-bottom_pos,
				TEXTBOX_FG, TEXTBOX_BG);
	}
	tb_set_cursor(get_input_cursor_pos(), height-bottom_pos);
	bottom_pos++;

	// Write the status line	
	const char* md = mode_desc();
	int mdl = strlen(md);
	for (int i=0; i<MIN(width, mdl); i++) {
		render_char(md[i], i, height-bottom_pos,
				STATUSLINE_FG, STATUSLINE_BG);
	}
	bottom_pos++;

	// Write the channels list, taking up the rest of the terminal height
	// update the window start if required.
	int max_chans = height - (bottom_pos-1);
	int conversation_selection_pos = get_conversation_selection_pos();
	int conversation_window_start = get_conversation_window_start();
	if ((conversation_selection_pos - conversation_window_start) >= max_chans) {
		set_conversation_window_start(conversation_selection_pos - (max_chans-1));
	} else if (conversation_selection_pos < conversation_window_start) {
		set_conversation_window_start(conversation_selection_pos);
	}

	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"select id, name "
				"from conversation "
				"where is_member = 1 "
				"order by name "
				"limit ? "
				"offset ? ", -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_int(stmt, 1, max_chans));
	sqlite_check(db, sqlite3_bind_int(stmt, 2, conversation_window_start))

	bool more = true;
	for (int j=0; j<max_chans; j++) {
		const char* id = "";
		const char* name = "";
		if (more) {
			int v = sqlite3_step(stmt);
			if (v == SQLITE_DONE) {
				more = false;
			} else if (v == SQLITE_ROW) {
				id = sqlite3_column_text(stmt, 0);
				name = sqlite3_column_text(stmt, 1);
			} else {
				sqlite_check(db, v);
			}
		} 
		bool selected = conversation_window_start + j == conversation_selection_pos;
		int namelen = strlen(name);
		for (int i=0; i<CHANS_WIDTH; i++) {
			char ch = i<namelen ? name[i] : ' ';
			int fg = selected ? CHANNELS_FG_SELECTED : CHANNELS_FG;
			int bg = selected ? CHANNELS_BG_SELECTED : CHANNELS_BG;
			render_char(ch, i, j, fg, bg);
		}
	}
	sqlite3_finalize(stmt);

	// Write the message list
	int max_messages = height - (bottom_pos-1);
	int user_start_x = CHANS_WIDTH;
	int message_start_x = CHANS_WIDTH + USER_WIDTH;
	int message_width = width - message_start_x;
	const char* selected_conversation_id = get_selected_conversation_id();
	if (selected_conversation_id != NULL) {
		sqlite_check(db, sqlite3_prepare_v2(db, 
					"select u.name, m.user, m.text, m.acknowledged "
					"from message m "
					"left join user u "
					  "on u.id = m.user "
					"where conversation = ? "
					"order by ts desc", -1, &stmt, NULL));
		sqlite_check(db, sqlite3_bind_text(stmt, 1, selected_conversation_id, -1, NULL));
		more = true;
		int msg_bg_col = MESSAGE_BG;
		int j = max_messages - 1;
		while (j >= 0) {
			if (more) {
				int v = sqlite3_step(stmt);
				if (v == SQLITE_DONE) {
					more = false;
				} else if (v == SQLITE_ROW) {
					const char* user = sqlite3_column_text(stmt, 0);
					if (user == NULL) {
						user = sqlite3_column_text(stmt, 1);
					}
					const char* text = sqlite3_column_text(stmt, 2);
					bool acked = sqlite3_column_int(stmt, 3);
					list_t* ll = to_utf8_list(text);
					list_t* lines = wrap(ll, message_width);
					int lines_len = list_size(lines);
					list_destroy(ll);

					for (int k=0; k<lines_len; k++) {
						list_t* line = list_get_at(lines, k);
						int line_len = list_size(line);
						char* line_chr = to_char_array(line);
						free(line_chr);
						int y = (j-lines_len) + 1 + k;

						const char* usrstr = k == 0 ? user : "";
						int usrstrlen = strlen(usrstr);
						// Add 1 padding for readability here.
						for (int i=1; i<USER_WIDTH; i++) {
							char ch = (i-1) < usrstrlen ? usrstr[i-1] : ' ';
							int x = i + user_start_x;
							render_char(ch, i+user_start_x, y, USER_FG, USER_BG);
						}

						for (int i=0; i<message_width; i++) {
							u_int32_t ch;
							if (i < line_len) {
								ch = *((u_int32_t*)list_get_at(line, i));
							} else {
								ch = ' ';
							}
							int x = i + message_start_x;
							render_char(ch, x, y, acked ? MESSAGE_FG : MESSAGE_FG_UNACKED, msg_bg_col);
						}
					}
					list_of_lists_destroy(lines);
					j -= lines_len;
					// toggle the background colour between messages
					msg_bg_col = msg_bg_col == MESSAGE_BG ? MESSAGE_BG_ALT : MESSAGE_BG;
				} else {
					sqlite_check(db, v);
				}
			} else {
				for (int i=0; i<USER_WIDTH; i++) {
					render_char(' ', i+user_start_x, j, USER_FG, USER_BG);
				}
				for (int i=0; i<message_width; i++) {
					render_char(' ', i+message_start_x, j, MESSAGE_FG, MESSAGE_BG);
				}
				j--;
			}
		}
		free((void*)selected_conversation_id);
		sqlite3_finalize(stmt);
	}

	tb_present();
}

void handle_event_mode_normal(struct tb_event* evt) {
	int conversation_selection_pos = get_conversation_selection_pos();
	switch (evt->ch) {
	case 'i':
		set_current_mode(mode_insert);
		return;
	case '/': 
		set_current_mode(mode_search);
		return;
	case 'w': 
		set_conversation_selection_pos(conversation_selection_pos-1);
		return;
	case 's': 
		set_conversation_selection_pos(conversation_selection_pos+1);
		return;
	case 'q': 
		quit = true;
		return;
	default:
		return;
	}
}

bool delete_input_buffer(int pos) {
	list_t* input_buffer = get_input_buffer();
	
	if (list_size(input_buffer) <= pos) {
		list_destroy(input_buffer);
		return false;
	} else if (pos < 0) {
		return false;
	}
	void* ch = list_get_at(input_buffer, pos);
	list_delete_at(input_buffer, pos);
	set_input_buffer(input_buffer);
	list_destroy(input_buffer);
	return true;
}

void insert_input_buffer(u_int32_t ch) {
	list_t* input_buffer = get_input_buffer();
	int input_cursor_pos = get_input_cursor_pos();
	list_insert_at(input_buffer, &ch, input_cursor_pos);
	set_input_buffer(input_buffer);
	list_destroy(input_buffer);
}

void send_pending_messages(struct state_update su) {
	if (ws_connection == NULL) {
		return;
	}
	if (strcmp(su.tablename, "message") != 0) {
		return;
	}
	if (su.operation != SQLITE_INSERT) {
		return;
	}
	// Find unsent messages
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_exec(db, "begin", NULL, NULL, NULL));
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"select json_object("
					"'id', id, "
					"'channel', conversation, "
					"'type', 'message', "
					"'text', text "
				") from message where pending = 1", -1, &stmt, NULL));
	int v;
	for (v = sqlite3_step(stmt); v == SQLITE_ROW; v = sqlite3_step(stmt)) {
		const char* payload = sqlite3_column_text(stmt, 0);
		dbg("sending message %s", payload);
		mg_ws_send(ws_connection, payload, strlen(payload), WEBSOCKET_OP_TEXT);
	}
	if (v != SQLITE_DONE) {
		sqlite_check(db, v);
	}
	sqlite_check(db, sqlite3_exec(db, "update message set pending = 0 where pending = 1", NULL, NULL, NULL));
	sqlite_check(db, sqlite3_exec(db, "commit", NULL, NULL, NULL));
/*	{
	"id": 2,
	"type": "message",
	"channel": "C024BE91L",
	"text": "Hello <@U123ABC>"
}*/
}

bool send_message() {
	char* current_user_id = get_current_user_id();
	if (ws_connection == NULL 
	  || current_user_id == NULL) {
		return false;
	}
	list_t* input_buffer = get_input_buffer();

	char ts[12];
	time_t t = time(NULL);
	snprintf(ts, 12, "%ld", t);
	char* text = to_char_array(input_buffer);
	const char* selected_conversation_id = get_selected_conversation_id();
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, "insert into message (conversation, type, user, text, ts, pending, acknowledged) "
				"values (?, ?, ?, ?, ?, ?, ?)", -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 1, selected_conversation_id, -1, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 2, "message", -1, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 3, current_user_id, -1, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 4, text, -1, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 5, ts, -1, NULL));
	sqlite_check(db, sqlite3_bind_int(stmt, 6, 1));
	sqlite_check(db, sqlite3_bind_int(stmt, 7, 0));
	sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
	sqlite3_finalize(stmt);

	free((void*)selected_conversation_id);
	free(current_user_id);
	free(text);
	list_destroy(input_buffer);

	return true;
}

void clear_input_buffer() {
	list_t* lst = new_copying_list(0);
	set_input_buffer(lst);
	set_input_cursor_pos(0);
}

void handle_editor(list_t* buffer,
		int* cursor_pos,
		const char* buffer_variable_name,
		const char* cursor_pos_variable_name) {

}

void handle_event_insert(struct tb_event* evt) {
	sqlite3_str* str = sqlite3_str_new(db);
	int input_cursor_pos = get_input_cursor_pos();
	list_t* input_buffer = get_input_buffer();
	if (evt->key == TB_KEY_ARROW_LEFT) {
		set_input_cursor_pos(input_cursor_pos-1);
	} else if (evt->key == TB_KEY_ARROW_RIGHT) {
		set_input_cursor_pos(input_cursor_pos+1);
	} else if (evt->key == TB_KEY_HOME) {
		set_input_cursor_pos(0);
	} else if (evt->key == TB_KEY_END) {
		set_input_cursor_pos(list_size(input_buffer));
	} else if (evt->key == TB_KEY_BACKSPACE 
	        || evt->key == TB_KEY_BACKSPACE2) {
		if (delete_input_buffer(input_cursor_pos-1)) {
			set_input_cursor_pos(input_cursor_pos-1);
		}
	} else if (evt->key == TB_KEY_DELETE) {
		delete_input_buffer(input_cursor_pos);
		if (list_size(input_buffer) < input_cursor_pos) {
			set_input_cursor_pos(input_cursor_pos-1);
		}
	} else if (evt->key == TB_KEY_ENTER) {
		if (send_message()) {
			clear_input_buffer();
		}
	} else {
		char ch;
		if (evt->key == TB_KEY_SPACE) {
			ch = ' ';
		} else {
			ch = evt->ch;
		}
		insert_input_buffer(ch);
		set_input_cursor_pos(input_cursor_pos+1);
	}
	list_destroy(input_buffer);
}

void handle_event_search(struct tb_event* evt) {
}

void handle_event(struct tb_event* evt) {
	if (evt->type == TB_EVENT_RESIZE) {
		list_append(&state_update_queue, database_update("", 0, "", -1));
		return;
	}
	if (evt->type == TB_EVENT_KEY) {
		if (evt->key == TB_KEY_ESC) {
			set_current_mode(mode_normal);
			return;
		}
		switch (get_current_mode()) {
		case mode_normal:
			handle_event_mode_normal(evt);
			return;
		case mode_insert:
			handle_event_insert(evt);
			return;
		case mode_search:
			handle_event_search(evt);
			return;
		}
	}
}

/*
 * Handles initializing TLS and adding the relevant AUTHORIZATION header
 */
static void handle_connect(const char* url, struct mg_connection* c) {
	// Connected to server
	struct mg_str host = mg_url_host(url);
	if (mg_url_is_ssl(url)) {
		// If s_url is https://, tell client connection to use TLS
		struct mg_tls_opts opts = {
			.ca = "/etc/ssl/cert.pem"
		};
		mg_tls_init(c, &opts);
	}
	// Send request
	mg_printf(c, "GET %s HTTP/1.0\r\n"
			"Host: %.*s\r\n"
			"Authorization: Bearer %s\r\n"
			"\r\n\r\n", 
			mg_url_uri(url),
			(int) host.len, host.ptr,
			getenv("SLACK_TOKEN"));
}

static void handle_conversations(struct mg_connection* c, int ev, void* ev_data, void* fn_data) {
	if (ev == MG_EV_CONNECT) {
		handle_connect(slack_conversations_list_url ,c);
	} else if (ev == MG_EV_HTTP_MSG) {
		struct mg_http_message* hm = (struct mg_http_message*)ev_data;
		sqlite_check(db, sqlite3_exec(db, "begin", NULL, NULL, NULL));
		sqlite_check(db, sqlite3_exec(db, "delete from conversation", NULL, NULL, NULL));
		sqlite3_stmt* stmt;
		sqlite_check(db, sqlite3_prepare_v2(db, 
					"insert into conversation "
					"(id, name, is_member) "
					"select "
						"json_extract(value, '$.id'), "
						"json_extract(value, '$.name'), "
						"json_extract(value, '$.is_member') "
					"from json_each(?, '$.channels')", -1, &stmt, NULL));
		sqlite_check(db, sqlite3_bind_text(stmt, 1, hm->body.ptr, hm->body.len, NULL));
		sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
		sqlite3_finalize(stmt);
		sqlite_check(db, sqlite3_exec(db, "commit", NULL, NULL, NULL));
		// Update the conversation position now we modified the conversation table
		set_conversation_selection_pos(get_conversation_selection_pos());
	} else if (ev == MG_EV_ERROR) {
		char* err = ev_data;
		dbg("Error fetching conversations %s", err);
	}

}

static void handle_users(struct mg_connection* c, int ev, void* ev_data, void* fn_data) {
	if (ev == MG_EV_CONNECT) {
		handle_connect(slack_users_list_url ,c);
	} else if (ev == MG_EV_HTTP_MSG) {
		struct mg_http_message* hm = (struct mg_http_message*)ev_data;
		sqlite_check(db, sqlite3_exec(db, "begin", NULL, NULL, NULL));
		sqlite_check(db, sqlite3_exec(db, "delete from user", NULL, NULL, NULL));
		sqlite3_stmt* stmt;
		sqlite_check(db, sqlite3_prepare_v2(db, 
					"insert into user "
					"(id, name) "
					"select "
						"json_extract(value, '$.id'), "
						"json_extract(value, '$.name') "
					"from json_each(?, '$.members')", -1, &stmt, NULL));
		sqlite_check(db, sqlite3_bind_text(stmt, 1, hm->body.ptr, hm->body.len, NULL));
		sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
		sqlite3_finalize(stmt);
		sqlite_check(db, sqlite3_exec(db, "commit", NULL, NULL, NULL));
	} else if (ev == MG_EV_ERROR) {
		char* err = ev_data;
		dbg("Error fetching users %s", err);
	}
}

void handle_ws_message(struct mg_str payload) {
	/*
	{
	"type": "message",
	"channel": "C2147483705",
	"user": "U2147483697",
	"text": "Hello world",
	"ts": "1355517523.000005"
	}
	*/
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"with js(c) as (select json(?)) "
				"insert into message "
				"(type, conversation, ts, user, text) "
				"select "
					"json_extract(c, '$.type'),"
					"json_extract(c, '$.channel'),"
					"json_extract(c, '$.ts'),"
					"json_extract(c, '$.user'),"
					"json_extract(c, '$.text')"
				, -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 1, payload.ptr, payload.len, NULL));
	sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
	sqlite3_finalize(stmt);
}

void handle_ws_hello(struct mg_str payload) {
	// List conversations
	mg_http_connect(&mgr, 
			slack_conversations_list_url, 
			handle_conversations, 
			NULL);
	// List users
	mg_http_connect(&mgr,
			slack_users_list_url,
			handle_users,
			NULL);
}

void handle_ws_reply(struct mg_str payload) {
	/*
	{
	"ok": true,
	"reply_to": 1,
	"ts": "1355517523.000005",
	"text": "Hello world"
	}
	*/
	/*
	{
	"ok": false,
	"reply_to": 1,
	"error": {
		"code": 2,
		"msg": "message text is missing"
	}
	}
	*/
	dbg("handling reply %.*s", payload.len, payload.ptr);
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"with js(c) as (select json(?)) "
				"update message "
				"set ts = json_extract(c, '$.ts'), "
				    "text = json_extract(c, '$.text'),"
				    "acknowledged = 1 "
				"from js "
				"where id = json_extract(c, '$.reply_to') "
				"and json_extract(c, '$.ok') == 1 "
				, -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 1, payload.ptr, payload.len, NULL));
	sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
	sqlite3_finalize(stmt);
}

static void handle_ws(struct mg_connection* c, int ev, void* ev_data, void* fn_data) {
	// Assign to global so we can use it for sending
	if (ev == MG_EV_CONNECT) {
		const char* url = (const char*)fn_data;
		handle_connect(url, c);
	} else if (ev == MG_EV_WS_MSG) {
		struct mg_ws_message* wm = (struct mg_ws_message*)ev_data;
		// If it's a hello, then we have a successful websocket connection
		// and we can proceed to make other data fetches.
		sqlite3_stmt* stmt;
		sqlite_check(db, sqlite3_prepare_v2(db, "with js(c) as (select json(?)) "
					"select "
						"json_extract(c, '$.type'), "
						"json_extract(c, '$.reply_to') "
					"from js", -1, &stmt, NULL));
		sqlite_check(db, sqlite3_bind_text(stmt, 1, wm->data.ptr, wm->data.len, NULL));
		int v = sqlite3_step(stmt);
		if (v == SQLITE_ROW) {
			const char* type = sqlite3_column_text(stmt, 0);
			int reply_to = sqlite3_column_int(stmt, 1);
			if (reply_to > 0) {
				handle_ws_reply(wm->data);
			} else if (type != NULL) {
				if (strcmp(type, "hello") == 0) {
					// Remember the new websocket connection so we can send stuff
					ws_connection = c;
					handle_ws_hello(wm->data);
				} else if (strcmp(type, "message") == 0) {
					handle_ws_message(wm->data);
				} else {
					dbg("unhandled message type %s", type);
				}
			} else {
				dbg("websocket message with no reply_to or type %.*s", wm->data.len, wm->data.ptr);
			}
		} else {
			sqlite_check(db, v);
		}
		sqlite3_finalize(stmt);
	}
}

static void handle_rtm_connect(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
	if (ev == MG_EV_CONNECT) {
		handle_connect(slack_rtm_connect_url ,c);
	} else if (ev == MG_EV_HTTP_MSG) {
		struct mg_http_message *hm = (struct mg_http_message *) ev_data;
		struct mg_str payload = hm->body;
		sqlite3_stmt* stmt;
		sqlite_check(db, sqlite3_prepare_v2(db, 
					"with js(c) as (select json(?)) "
					"select json_extract(c, '$.url'), "
						"json_extract(c, '$.self.id') "
					"from js"
					, -1, &stmt, NULL));
		sqlite_check(db, sqlite3_bind_text(stmt, 1, payload.ptr, payload.len, NULL));
		struct json_tokener* t = json_tokener_new();
		struct json_object* start_response = json_tokener_parse_ex(t, hm->body.ptr, hm->body.len);
		struct json_object* wss_url_json;
		struct json_object* self_json; 
		struct json_object* self_id_json;
		if (json_object_object_get_ex(start_response, "url", &wss_url_json)) {
			const char* wss_url = json_object_get_string(wss_url_json);
			mg_ws_connect(&mgr, wss_url, handle_ws, (void*)wss_url, NULL);
		}
		if (json_object_object_get_ex(start_response, "self", &self_json)
		  && json_object_object_get_ex(self_json, "id", &self_id_json)) {
			set_current_user_id(strdup(json_object_get_string(self_id_json)));
		}
		json_object_put(start_response);
		json_tokener_free(t);
		c->is_closing = 1;
	} 
}

void cleanup() {
	mg_mgr_free(&mgr);
	tb_shutdown();
	fclose(errfile);
	fclose(dbgfile);
	sqlite3_close(db);
}

static volatile sig_atomic_t sigint_in_progress = 0;
void handle_term(int sig) {
	if (sigint_in_progress) {
		raise(sig);
		return;
	}
	sigint_in_progress = 1;
	cleanup();
	signal(sig, SIG_DFL);
	raise(sig);
}

void update_hook(void* user_data, 
		int operation, 
		const char* database, 
		const char* tablename, 
		sqlite3_int64 rowid) {
	struct state_update* u = database_update(database,
			operation, 
			tablename, 
			rowid);
	list_append(&state_update_queue, u);
}

static void handle_conversation_history(struct mg_connection* c, int ev, void* ev_data, void* fn_data) {
	char* selected_conversation_id = fn_data;
	if (ev == MG_EV_CONNECT) {
		char* url = format_url1(slack_conversation_history_url, selected_conversation_id);
		handle_connect(url, c);
		free(url);
	} else if (ev == MG_EV_HTTP_MSG) {
		struct mg_http_message* hm = ev_data;
		struct json_tokener* tok = json_tokener_new();
		struct json_object* jo = json_tokener_parse_ex(tok, hm->body.ptr, hm->body.len);
		struct json_object* messages_json;
		if (json_object_object_get_ex(jo, "messages", &messages_json)) {
			sqlite_check(db, sqlite3_exec(db, "begin", NULL, NULL, NULL));
			sqlite3_stmt* stmt;
			sqlite_check(db, sqlite3_prepare_v2(db, "delete from message where conversation = ?", -1, &stmt, NULL));	
			sqlite_check(db, sqlite3_bind_text(stmt, 1, selected_conversation_id, -1, NULL));
			sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
			sqlite3_finalize(stmt);
			sqlite_check(db, sqlite3_prepare_v2(db, "insert into message (conversation, type, user, text, ts) values (?, ?, ?, ?, ?)", -1, &stmt, NULL));
			int len = json_object_array_length(messages_json);
			for (int i=0; i<len; i++) {
				struct json_object* message_json = json_object_array_get_idx(messages_json, i);
				struct json_object* channel_json;
				struct json_object* type_json;
				struct json_object* user_json;
				struct json_object* text_json;
				struct json_object* ts_json;
				if (
				     // Slack don't provide this when you call conversation.history
				     // json_object_object_get_ex(message_json, "channel", &channel_json) &&
				  json_object_object_get_ex(message_json, "type", &type_json) &&
				  json_object_object_get_ex(message_json, "user", &user_json) &&
				  json_object_object_get_ex(message_json, "text", &text_json) &&
				  json_object_object_get_ex(message_json, "ts", &ts_json)) { 
					sqlite_check(db, sqlite3_reset(stmt));
					sqlite_check(db, sqlite3_clear_bindings(stmt));
					sqlite_check(db, sqlite3_bind_text(stmt, 1, selected_conversation_id, -1, NULL));
					sqlite_check(db, sqlite3_bind_text(stmt, 2, json_object_get_string(type_json), -1, NULL));
					sqlite_check(db, sqlite3_bind_text(stmt, 3, json_object_get_string(user_json), -1, NULL));
					sqlite_check(db, sqlite3_bind_text(stmt, 4, json_object_get_string(text_json), -1, NULL));
					sqlite_check(db, sqlite3_bind_text(stmt, 5, json_object_get_string(ts_json), -1, NULL));
					sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
				} else {
					dbg("Invalid message received %s", json_object_to_json_string(message_json));
				}
			}
			sqlite3_finalize(stmt);
			sqlite_check(db, sqlite3_exec(db, "commit", NULL, NULL, NULL));
		}
		json_object_put(jo);
		json_tokener_free(tok);
		c->is_closing = true;
		free(selected_conversation_id);
	} else if (ev == MG_EV_ERROR) {
		char* error_message = ev_data;
		dbg("Error fetching conversation history %s", error_message);
		c->is_closing = true;
		free(selected_conversation_id);
	}
}

/*
 * If a conversation is selected, check if we have the history.
 * If we don't, then fetch it.
 */
void fetch_conversation(struct state_update* u) {
	if (strcmp(u->tablename, "kvs") != 0) {
		return;
	}
	char* key = get_key_value_key_by_rowid(u->rowid);
	if (key == NULL || strcmp(key, "conversation_selection_pos") != 0){
		free(key);
		return;
	}
	free(key);
	const char* selected_conversation_id = get_selected_conversation_id();
	if  (get_conversation_did_fetch(selected_conversation_id)) {
		free((void*)selected_conversation_id);
		return;
	}
	char* url = format_url1(slack_conversation_history_url, selected_conversation_id);
	mg_http_connect(&mgr, url, handle_conversation_history, (void*)selected_conversation_id);
	set_conversation_did_fetch(selected_conversation_id, true);
	free(url);
}

bool process_state_update_queue() {
	bool did_process = false;
	for (struct state_update* u = list_extract_at(&state_update_queue, 0);
			u != NULL;
			u = list_extract_at(&state_update_queue, 0)) {
		did_process = true;
		for (int i=0; i<list_size(&state_listeners); i++) {
			void (*fn)(struct state_update*) = list_get_at(&state_listeners,i);
			fn(u);
		}
		free_state_update(u);
	}
	return did_process;
}

void init_database() {
	if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
		fprintf(errfile, "Failed to open database %s", sqlite3_errmsg(db));
		raise(SIGTERM);
	}
	const char* init_script =
				// Generic key-value store! Don't specify a datatype
				"create table if not exists kvs (key text primary key, value);"

				"create table if not exists conversation "
				"(id text, name text, is_member int, json text, did_fetch int default 0);"
				"create index if not exists idx_conversation_id on conversation(id);"

				"create table if not exists user (id text, name text);"

				"create table if not exists message "
				"(conversation text, "
				 "type text, "
				 "user text, "
				 "text text, "
				 "ts text, "
				 "id integer primary key autoincrement, "
				 "pending int default 0, "
				 "acknowledged int default 1)";
	sqlite_check(db, sqlite3_exec(db, init_script, NULL, NULL, NULL));
}

int main(int argc, const char** argv) {
	// Setup log files
	errfile = fopen("err.log", "w");
	dbgfile = fopen("dbg.log", "w");

	// Setup termination signal handling
	signal(SIGINT, handle_term);
	signal(SIGTERM, handle_term);

	// Setup sqlite
	init_database();

	// Initialize the processing queue
	list_init(&state_update_queue);
	list_init(&state_listeners);

	// Register state_listeners
	list_append(&state_listeners, fetch_conversation);
	list_append(&state_listeners, send_pending_messages);

	// Install update hook
	sqlite3_update_hook(db, update_hook, NULL);

	// Setup termbox
	tb_init();
	tb_clear();
	tb_set_clear_attributes(232, 255);
	tb_present();
	tb_select_input_mode(TB_INPUT_ESC);
	tb_select_output_mode(TB_OUTPUT_256);

	// Setup initial network connection
	ws_connection = NULL;
	mg_mgr_init(&mgr);
	mg_http_connect(&mgr, 
			slack_rtm_connect_url,
			handle_rtm_connect,
			NULL);

	// Render at least once on startup
	render(); 

	quit = false;
	while (!quit) {
		mg_mgr_poll(&mgr, 10);

		struct tb_event evt;
		if (tb_peek_event(&evt, 10) > 0) {
			handle_event(&evt);
		}
		
		if (process_state_update_queue()) {
			render();
		}
	}

	cleanup();
	return 0;
}
