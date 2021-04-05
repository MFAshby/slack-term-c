slack-term-c
============

Terminal slack client, inspired by [slack-term](https://github.com/erroneousboat/slack-term)
but written in C and sqlite because... why not?

## Build and Run

Works on linux, YMMV on any other platform.

Dependencies: tcc, libc, sqlite3. Install these through whatever package manager you use.

Just execute the bash script.
```bash
./run.sh
```

You can probably build and run in separate steps with gcc if you don't want to install tcc.

## User guide

Get a slack token via the [method described in slack-term](https://github.com/erroneousboat/slack-term/wiki#running-slack-term-without-legacy-tokens)

Put it in an environment variable `SLACK_TOKEN` before you execute run.sh

slack-term-c uses modes similar to vi, which change what the keyboard does. The current mode is displayed
at the bottom of the screen.

*Keyboard controls in normal mode:*
- q - quit
- i - enter 'insert mode'
- / - enter 'search mode'
- s - select next channel down
- w - select next channel up

*Keyboard controls in insert mode:*
- type to compose your message
- esc   - return to normal mode
- enter - send a message

*Keyboard controls in search mode:*
_search mode is not yet functional_
- type to enter search query
- esc - return to normal mode

## Architecture

Sqlite3 manages all the heavy lifting. Basic principals:
- all data and most application state is stored in sqlite.
- main loop polls network then user interface, executing 
  any callbacks, which will typically insert or update data in sqlite.
- an update hook collects updates to a linked list in memory.
- functions can be registered to react to changes in the database
- on each main loop iteration where anything changed in the database, the UI is updated.

The reason for the update queue is that the sqlite update hook can't modify the 
database at all, so we have to defer executing any code which might do that.

Reactions can be anything, for example trigging an http call to fetch conversation
history, or triggering a websocket message to send a message.

This queue approach allows decoupling, allowing complex behaviour while keeping 
individual functions very simple.

### Data structure

Data from slack is stored in tables closely following the API documentation.

Singleton data, like the currently selected conversation index, is stored in a table of
key-value pairs.

## TODO 
This project is missing a lot of features to make it actually useful.
- Direct Messages
- Message scrollback
- Loading indicator
- Search
- custom emoji / images 
- attachments
- mouse control
- @user 
- threading 
- attachment preview
- desktop notification
- unread indicator

## Attribution
This project draws from a few other open source projects. Some code is vendored where
that approach is recommended by the authors.

- [termbox](https://github.com/termbox/termbox) is used for terminal user interface.
- [mongoose](https://github.com/cesanta/mongoose) is used for networking.
- [sqlite3](https://www.sqlite.org/index.html) is used for data storage.
- [simclist](https://github.com/mij/simclist) is used for linked-list implementation.
