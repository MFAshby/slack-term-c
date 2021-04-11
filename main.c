#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>

#include <sqlite3.h>

#include "termbox.h"
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
		const char* sqlite_msg = sqlite3_errmsg(db); \
		if (sqlite_msg != NULL) { \
			fprintf(errfile, "sqlite3 error at: %s:%d\n" \
			   "%s", __FILE__, __LINE__, sqlite_msg); \
		} else { \
			fprintf(errfile, "sqlite3 error null message at: %s:%d\n" \
			   , __FILE__, __LINE__); \
		} \
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

// We'll use multiple input buffers
struct input_buffer {
	const char* buffer_key;
	const char* cursor_key;
};
struct input_buffer message_input_buffer = {
	.buffer_key = "message_input_buffer",
	.cursor_key = "message_input_cursor_pos",
};
struct input_buffer search_input_buffer = {
	.buffer_key = "search_input_buffer",
	.cursor_key = "search_input_cursor_pos",
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

/**
 * Singleton values (like UI selections, current user identity) are
 * stored in a special table of key-value pairs.
 */
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
		res = sqlite3_column_text(stmt, 0);
	} else if (v == SQLITE_DONE) {
		res = default_value;
	} else {
		sqlite_check(db, v);
	}
	if (res != NULL) {
		res = strdup(res);
	}
	sqlite3_finalize(stmt);
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
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"select count(1) "
				"from conversation_list ", 
				-1, &stmt, NULL));
	sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_ROW);
	int count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

void set_selected_conversation(char* new_selection) {
	set_key_value_string("selected_conversation", new_selection);
}
// Caller should free
char* get_selected_conversation() {
	return get_key_value_string("selected_conversation", NULL);
}

void set_conversation_window_start(int new_window_start) {
	// Bounds check
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


int get_conversation_selection_pos() {
	char* selected_conversation = get_selected_conversation();
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"select idx from conversation_list where id = ? "
				, -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 1, selected_conversation, -1, NULL));
	int v = sqlite3_step(stmt);
	int res = 0;
	if (v  == SQLITE_ROW) {
		res = sqlite3_column_int(stmt, 0);
	} else if (v  != SQLITE_DONE) {
		sqlite_check(db, v);
	}
	sqlite3_finalize(stmt);
	free(selected_conversation);
	return res;
}

void select_first_conversation() {
	const char* to_select = NULL;
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
			"select id "
			"from conversation_list "
			"order by display_name "
			"limit 1", -1, &stmt, NULL));
	int v = sqlite3_step(stmt);
	if (v == SQLITE_ROW) {
		to_select = sqlite3_column_text(stmt, 0);
	} else if (v != SQLITE_DONE) {
		sqlite_check(db, v);
	}

	if (to_select != NULL) {
		set_selected_conversation(to_select);
	}
	sqlite3_finalize(stmt);
}

void select_conversation(bool next) {
	char buf[200];
	char* selected_conversation = get_selected_conversation();
	sqlite3_stmt* stmt;
	if (selected_conversation != NULL) {
		char* to_select = NULL;
		snprintf(buf, 200, 
			"select %s "
			"from conversation_list "
			"where id = ?", next ? "next" : "prev");
		dbg("executing %s", buf);
		sqlite_check(db, sqlite3_prepare_v2(db, buf, -1, &stmt, NULL));
		sqlite_check(db, sqlite3_bind_text(stmt, 1, selected_conversation, -1, NULL));
		int v = sqlite3_step(stmt);
		if (v == SQLITE_ROW) {
			to_select = sqlite3_column_text(stmt, 0);
		} else if (v != SQLITE_DONE) {
			sqlite_check(db, v);
		}

		if (to_select != NULL) {
			set_selected_conversation(to_select);
		} else {
			select_first_conversation();
		}
	} else {
		select_first_conversation();
	}
	sqlite3_finalize(stmt);
	free(selected_conversation);
}

void select_previous_conversation() {
	select_conversation(false);
}

void select_next_conversation() {
	select_conversation(true);
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
	free(list);
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

void set_input_buffer(list_t* lst, struct input_buffer b) {
	char* ib = to_char_array(lst);
	set_key_value_string(b.buffer_key, ib);
	free(ib);
}
list_t* get_input_buffer(struct input_buffer b) {
	char* str = get_key_value_string(b.buffer_key, "");
	list_t* ret = to_utf8_list(str);
	free(str);
	return ret;
}

void set_input_cursor_pos(int n, struct input_buffer b) {
	list_t* input_buffer = get_input_buffer(b);
	int max = list_size(input_buffer);
	list_destroy(input_buffer);
	if (n < 0) {
		return;
	} else if (n > max) {
		return;
	}
	set_key_value_int(b.cursor_key, n);
}
int get_input_cursor_pos(struct input_buffer b) {
	return get_key_value_int(b.cursor_key, 0);
}

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

/**
 * Inspired by https://stackoverflow.com/questions/22582989/word-wrap-program-c
 * Returns a list of lists.
 */
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
	struct input_buffer b;
	int mode = get_current_mode();
	switch (mode) {
		case mode_normal:
		case mode_insert:
			b = message_input_buffer;
			break;
		case mode_search:
			b = search_input_buffer;
			break;
	}
	list_t* input_buffer = get_input_buffer(b);
	int cursor_pos = get_input_cursor_pos(b);
	int input_buffer_len = list_size(input_buffer);
	for (int i=0; i<MIN(width, input_buffer_len); i++) {
		u_int32_t* ch = list_get_at(input_buffer, i);
		render_char(*ch, i, height-bottom_pos,
				TEXTBOX_FG, TEXTBOX_BG);
	}
	tb_set_cursor(cursor_pos, height-bottom_pos);
	bottom_pos++;

	// Write the status line	
	const char* md = mode_desc();
	int mdl = strlen(md);
	for (int i=0; i<MIN(width, mdl); i++) {
		render_char(md[i], i, height-bottom_pos,
				STATUSLINE_FG, STATUSLINE_BG);
	}
	bottom_pos++;

	// Recalculate the display for channels list
	int max_chans = height - (bottom_pos-1);
	int conversation_selection_pos = get_conversation_selection_pos();
	int conversation_window_start = get_conversation_window_start();
	if ((conversation_selection_pos - conversation_window_start) >= max_chans) {
		set_conversation_window_start(conversation_selection_pos - (max_chans-1));
	} else if (conversation_selection_pos < conversation_window_start) {
		set_conversation_window_start(conversation_selection_pos);
	}

	const char* selected_conversation_id = get_selected_conversation();
	sqlite3_stmt* stmt;
	sqlite_check(db, sqlite3_prepare_v2(db, 
				"select id, display_name "
				"from conversation_list "
				"order by display_name "
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
		bool selected = selected_conversation_id != NULL && strcmp(id, selected_conversation_id) == 0;
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
					if (user == NULL) {
						user = "unknown!";
					}
					const char* text = sqlite3_column_text(stmt, 2);
					bool acked = sqlite3_column_int(stmt, 3);
					list_t* ll = to_utf8_list(text);
					list_t* lines = wrap(ll, message_width);
					list_destroy(ll);
					free(ll);

					int lines_len = list_size(lines);
					for (int k=0; k<lines_len; k++) {
						list_t* line = list_get_at(lines, k);
						int line_len = list_size(line);
						int y = (j-lines_len) + 1 + k;

						const char* usrstr = k == 0 ? user : "";
						int usrstrlen = strlen(usrstr);
						// Add 1 padding for readability here.
						for (int i=1; i<USER_WIDTH; i++) {
							char ch = (i-1) < usrstrlen ? usrstr[i-1] : ' ';
							int x = i + user_start_x;
							render_char(ch, i+user_start_x, y, USER_FG, msg_bg_col);
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
	switch (evt->ch) {
	case 'i':
		set_current_mode(mode_insert);
		return;
	case '/': 
		set_current_mode(mode_search);
		return;
	case 'w': 
		select_previous_conversation();
		return;
	case 's':
		select_next_conversation();
		return;
	case 'q': 
		quit = true;
		return;
	default:
		return;
	}
}

bool delete_input_buffer(int pos, struct input_buffer b) {
	list_t* ib = get_input_buffer(b);
	
	if (list_size(ib) <= pos) {
		list_destroy(ib);
		return false;
	} else if (pos < 0) {
		return false;
	}
	void* ch = list_get_at(ib, pos);
	list_delete_at(ib, pos);
	set_input_buffer(ib, b);
	list_destroy(ib);
	return true;
}

void insert_input_buffer(u_int32_t ch, struct input_buffer b) {
	list_t* ib = get_input_buffer(b);
	int input_cursor_pos = get_input_cursor_pos(b);
	list_insert_at(ib, &ch, input_cursor_pos);
	set_input_buffer(ib, b);
	list_destroy(ib);
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
	sqlite3_finalize(stmt);
	sqlite_check(db, sqlite3_exec(db, "update message set pending = 0 where pending = 1", NULL, NULL, NULL));
	sqlite_check(db, sqlite3_exec(db, "commit", NULL, NULL, NULL));
}

bool send_message(struct input_buffer b) {
	char* current_user_id = get_current_user_id();
	if (ws_connection == NULL 
	  || current_user_id == NULL) {
		return false;
	}
	list_t* ib = get_input_buffer(b);

	char ts[12];
	time_t t = time(NULL);
	snprintf(ts, 12, "%ld", t);
	char* text = to_char_array(ib);
	const char* selected_conversation_id = get_selected_conversation();
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
	list_destroy(ib);

	return true;
}

void clear_input_buffer(struct input_buffer b) {
	list_t* lst = new_copying_list(0);
	set_input_buffer(lst, b);
	set_input_cursor_pos(0, b);
}

void handle_editor(list_t* buffer,
		int* cursor_pos,
		const char* buffer_variable_name,
		const char* cursor_pos_variable_name) {

}

void update_input_buffer(struct tb_event* evt, 
		struct input_buffer b,
		void (*enter_callback)(struct input_buffer b)) {
	sqlite3_str* str = sqlite3_str_new(db);
	int input_cursor_pos = get_input_cursor_pos(b);
	list_t* ib = get_input_buffer(b);
	if (evt->key == TB_KEY_ARROW_LEFT) {
		set_input_cursor_pos(input_cursor_pos-1, b);
	} else if (evt->key == TB_KEY_ARROW_RIGHT) {
		set_input_cursor_pos(input_cursor_pos+1, b);
	} else if (evt->key == TB_KEY_HOME) {
		set_input_cursor_pos(0, b);
	} else if (evt->key == TB_KEY_END) {
		set_input_cursor_pos(list_size(ib), b);
	} else if (evt->key == TB_KEY_BACKSPACE 
	        || evt->key == TB_KEY_BACKSPACE2) {
		if (delete_input_buffer(input_cursor_pos-1, b)) {
			set_input_cursor_pos(input_cursor_pos-1, b);
		}
	} else if (evt->key == TB_KEY_DELETE) {
		delete_input_buffer(input_cursor_pos, b);
		if (list_size(ib) < input_cursor_pos) {
			set_input_cursor_pos(input_cursor_pos-1, b);
		}
	} else if (evt->key == TB_KEY_ENTER) {
		enter_callback(b);
	} else {
		char ch;
		if (evt->key == TB_KEY_SPACE) {
			ch = ' ';
		} else {
			ch = evt->ch;
		}
		insert_input_buffer(ch, b);
		set_input_cursor_pos(input_cursor_pos+1, b);
	}
	list_destroy(ib);
}

void send_and_clear(struct input_buffer b) {
	if (send_message(b)) {
		clear_input_buffer(b);
	}
}

void handle_event_insert(struct tb_event* evt) {
	update_input_buffer(evt, message_input_buffer, send_and_clear);
}

void handle_event_search(struct tb_event* evt) {
	update_input_buffer(evt, search_input_buffer, send_and_clear);
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
					"(id, name, is_member, is_im, user) "
					"select "
						"json_extract(value, '$.id'), "
						"json_extract(value, '$.name'), "
						"json_extract(value, '$.is_member'), "
						"json_extract(value, '$.is_im'), "
						"json_extract(value, '$.user') "
					"from json_each(?, '$.channels')", -1, &stmt, NULL));
		sqlite_check(db, sqlite3_bind_text(stmt, 1, hm->body.ptr, hm->body.len, NULL));
		sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
		sqlite3_finalize(stmt);
		sqlite_check(db, sqlite3_exec(db, "commit", NULL, NULL, NULL));
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
					"json_extract(c, '$.text') "
				"from js"
				, -1, &stmt, NULL));
	sqlite_check(db, sqlite3_bind_text(stmt, 1, payload.ptr, payload.len, NULL));
	sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
	sqlite3_finalize(stmt);
}

void handle_ws_hello(struct mg_str payload) {
	mg_http_connect(&mgr, 
			slack_conversations_list_url, 
			handle_conversations, 
			NULL);
	mg_http_connect(&mgr,
			slack_users_list_url,
			handle_users,
			NULL);
}

void handle_ws_reply(struct mg_str payload) {
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
	if (ev == MG_EV_CONNECT) {
		const char* url = (const char*)fn_data;
		handle_connect(url, c);
	} else if (ev == MG_EV_WS_MSG) {
		struct mg_ws_message* wm = (struct mg_ws_message*)ev_data;
		sqlite3_stmt* stmt;
		sqlite_check(db, sqlite3_prepare_v2(db, 
					"with js(c) as (select json(?)) "
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
		sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_ROW);
		const char* wss_url = sqlite3_column_text(stmt, 0);
		const char* current_user_id = sqlite3_column_text(stmt, 1);
		// Need to take a copy of the dynamic URL for the connect method later
		mg_ws_connect(&mgr, wss_url, handle_ws, (void*)strdup(wss_url), NULL);
		set_current_user_id(current_user_id);
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
		dbg("handing conversation history %.*s", hm->body.len, hm->body.ptr);

		sqlite_check(db, sqlite3_exec(db, "begin", NULL, NULL, NULL));
		sqlite3_stmt* stmt;
		sqlite_check(db, sqlite3_prepare_v2(db, "delete from message where conversation = ?", -1, &stmt, NULL));	
		sqlite_check(db, sqlite3_bind_text(stmt, 1, selected_conversation_id, -1, NULL));
		sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
		sqlite3_finalize(stmt);

		sqlite_check(db, sqlite3_prepare_v2(db, 
					"insert into message "
					"(conversation, type, user, text, ts) "
					"select "
						"?, "
						"json_extract(value, '$.type'), "
						"json_extract(value, '$.user'), "
						"json_extract(value, '$.text'), "
						"json_extract(value, '$.ts') "
					"from json_each(?, '$.messages')", -1, &stmt, NULL));
		sqlite_check(db, sqlite3_bind_text(stmt, 1, selected_conversation_id, -1, NULL));
		sqlite_check(db, sqlite3_bind_text(stmt, 2, hm->body.ptr, hm->body.len, NULL));
		sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
		sqlite3_finalize(stmt);
		sqlite_check(db, sqlite3_exec(db, "commit", NULL, NULL, NULL));
		c->is_closing = true;
		free(selected_conversation_id);
	} else if (ev == MG_EV_ERROR) {
		char* error_message = ev_data;
		dbg("Error fetching conversation history %s", error_message);
		c->is_closing = true;
		free(selected_conversation_id);
	}
}

// Build up the conversations list for left hand panel
// If the conversation table changes, or the search input buffer
void update_conversations_list(struct state_update* u) {
	if (strcmp(u->tablename, "kvs") != 0
	  && strcmp(u->tablename, "conversation") != 0) {
		return;
	}
	if (strcmp(u->tablename, "kvs") ==  0) {
		char* key = get_key_value_key_by_rowid(u->rowid);
		if (key == NULL || strcmp(key, "search_input_buffer") != 0){
			free(key);
			return;
		}
		free(key);
	}
	sqlite_check(db, sqlite3_exec(db, 
		"delete from conversation_list", NULL, NULL, NULL));
	list_t* sb = get_input_buffer(search_input_buffer);
	if (list_size(sb) > 0) {
		sqlite3_str* str = sqlite3_str_new(db);
		char* sc = to_char_array(sb);
		sqlite3_str_appendall(str, sc);
		sqlite3_str_appendchar(str, 1, '%');
		char* p = sqlite3_str_finish(str);
		free(sc);
		sqlite3_stmt* stmt;
		sqlite_check(db, sqlite3_prepare_v2(db, 
			"insert into conversation_list (id, next, prev, idx, display_name) "
			"with tmp(id, display_name, is_member, is_im) as ("
				"select c.id, "
					"case when is_im = 1 then u.name else c.name end as display_name, "
					"c.is_member, "
					"c.is_im "
				"from conversation c "
				"left outer join user u on u.id = c.user "
			")"
			"select id, "
				"lead(id, 1) over win, "
				"lag(id, 1) over win, "
				"(row_number() over win) - 1, "
				"display_name "
			"from tmp "
			"where (is_member = 1 or is_im = 1) "
			"and display_name like ? "
			"window win as (order by display_name) "
			, -1, &stmt, NULL));
		sqlite_check(db, sqlite3_bind_text(stmt, 1, p, -1, NULL));
		sqlite_check_ex(db, sqlite3_step(stmt), SQLITE_DONE);
		sqlite3_free(p);
		sqlite3_finalize(stmt);
	} else {
		sqlite_check(db, sqlite3_exec(db, 
			"insert into conversation_list (id, next, prev, idx, display_name) "
			"with tmp(id, display_name, is_member, is_im) as ("
				"select c.id, "
					"case when is_im = 1 then u.name else c.name end as display_name, "
					"c.is_member, "
					"c.is_im "
				"from conversation c "
				"left outer join user u on u.id = c.user "
			")"
			"select id, "
				"lead(id, 1) over win, "
				"lag(id, 1) over win, "
				"(row_number() over win) - 1, "
				"display_name "
			"from tmp "
			"where (is_member = 1 or is_im = 1) "
			"window win as (order by display_name) "
			, NULL, NULL, NULL));
	}
	list_destroy(sb);
	
}

void fetch_selected_conversation(struct state_update* u) {
	if (strcmp(u->tablename, "kvs") != 0) {
		return;
	}
	char* key = get_key_value_key_by_rowid(u->rowid);
	if (key == NULL || strcmp(key, "selected_conversation") != 0){
		free(key);
		return;
	}
	free(key);
	const char* selected_conversation_id = get_selected_conversation();
	if  (get_conversation_did_fetch(selected_conversation_id)) {
		free((void*)selected_conversation_id);
		return;
	}
	char* url = format_url1(slack_conversation_history_url, selected_conversation_id);
	mg_http_connect(&mgr, url, handle_conversation_history, (void*)selected_conversation_id);
	set_conversation_did_fetch(selected_conversation_id, true);
	free(url);
}

void reset_search(struct state_update* u) {
	if (strcmp(u->tablename, "kvs") != 0) {
		return;
	}
	char* key = get_key_value_key_by_rowid(u->rowid);
	if (key == NULL || strcmp(key, "mode") != 0){
		free(key);
		return;
	}
	free(key);
	if (get_current_mode() == mode_search) {
		list_t lst;
		list_init(&lst);
		set_input_buffer(&lst, search_input_buffer);
		set_input_cursor_pos(0, search_input_buffer);
	}
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

				// Conversations
				"create table if not exists conversation "
				"(id text, "
				"name text, "
				"is_member int, "
				"is_im int, "
				"user text, "
				"did_fetch int default 0);"
				"create index if not exists idx_conversation_id on conversation(id);"

				// View model of conversations
				"create table if not exists conversation_list "
				"(id text, "
				"next text, "
				"prev text, "
				"idx int, "
				"display_name text); "
				"create index if not exists idx_conversation_list_id on conversation_list(id);"

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
	list_append(&state_listeners, fetch_selected_conversation);
	list_append(&state_listeners, send_pending_messages);
	list_append(&state_listeners, update_conversations_list);
	list_append(&state_listeners, reset_search);

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
