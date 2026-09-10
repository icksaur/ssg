# Commands

Generated from the command catalog by `test_commands`. Do not
edit: change the command's registration instead, then regenerate
with `SSG_UPDATE_DOCS=1 ./build/test_commands`.

`init.lua` may call the commands marked `init.lua`; the rest are
available to the Lua API when a host grants them.

There are 162 commands.

## clipboard-register

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `clipboard.copy` | Copy | none | lua |
| `clipboard.cut` | Cut | none | lua |
| `clipboard.paste` | Paste | none | lua |

## edit-command-suite

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `edit.indent` | Indent | none | lua |
| `edit.outdent` | Outdent | none | lua |
| `edit.duplicate_line` | Duplicate Line | none | lua |
| `edit.move_line_up` | Move Line Up | none | lua |
| `edit.move_line_down` | Move Line Down | none | lua |
| `edit.delete_line` | Delete Line | none | lua |
| `edit.join_lines` | Join Lines | none | lua |
| `edit.uppercase` | Uppercase | none | lua |
| `edit.lowercase` | Lowercase | none | lua |
| `edit.swap_case` | Swap Case | none | lua |
| `edit.sort_lines` | Sort Lines | none | lua |
| `edit.transpose` | Transpose | none | lua |
| `edit.toggle_comment` | Toggle Comment | none | lua |

## external-modification-flow

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `external.reload` | Reload | none | lua |
| `external.keep_buffer` | Keep Buffer | none | lua |
| `external.open_diff` | Open Diff | none | lua |
| `external.select_next` | Select Next External Change | none | lua |
| `external.select_previous` | Select Previous External Change | none | lua |
| `external.focus` | Focus External Change Bar | none | lua |
| `external.focus_return` | Leave External Change Bar | none | lua |

## file-commands

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `workspace.open_directory` | Open Directory | none | lua |
| `file.new` | New File | none | lua |
| `file.open` | Open File | none | lua |
| `file.save` | Save File | none | lua |
| `file.save_all` | Save All Files | none | lua |
| `file.save_as` | Save File As | none | lua |
| `file.reload` | Reload File | none | lua |
| `file.rename` | Rename File | none | lua |
| `file.delete` | Delete File | none | lua |
| `file.new_directory` | New Directory | none | lua |

## find-replace

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `find.open` | Find | none | lua |
| `find.word_under_cursor` | Find Word Under Cursor | none | lua |
| `find.close` | Close | none | lua |
| `find.next` | Next | none | lua |
| `find.previous` | Previous | none | lua |
| `find.toggle_case` | Toggle Case | none | lua |
| `find.toggle_whole_word` | Toggle Whole Word | none | lua |
| `find.toggle_regex` | Toggle Regex | none | lua |
| `find.toggle_selection` | Toggle Selection | none | lua |
| `replace.open` | Replace | none | lua |
| `replace.current` | Current | none | lua |
| `replace.all` | All | none | lua |

## follow-edits

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `follow_edits.resume` | Resume | none | lua |
| `follow_edits.pause` | Pause | none | lua |
| `follow_edits.toggle` | Toggle | none | lua |

## help-system

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `help.open` | Open Help | none | lua |

## keymap-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `keymap.bind` | Bind | none | lua, init.lua |
| `keymap.unbind` | Unbind | none | lua, init.lua |

## lsp-language-features

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `goto.definition` | Go to Definition | none | lua |
| `goto.reference` | Reference | none | lua |
| `completion.open` | Open | none | lua |
| `completion.next` | Next | none | lua |
| `completion.previous` | Previous | none | lua |
| `completion.accept` | Accept | none | lua |
| `completion.dismiss` | Dismiss | none | lua |
| `hover.show` | Show | none | lua |
| `hover.dismiss` | Dismiss | none | lua |

## prompt-status-surface

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `prompt.submit` | Submit Prompt | none | lua |
| `prompt.cancel` | Cancel | none | lua |
| `prompt.next` | Next | none | lua |
| `prompt.previous` | Previous | none | lua |
| `prompt.focus_next_control` | Focus Next Field | none | lua |

## search-palette

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `palette.open` | Command Palette | none | lua |
| `file_finder.open` | Open | none | lua |
| `file_finder.toggle_gitignore` | Toggle Gitignore | none | lua |
| `palette.next` | Next | none | lua |
| `palette.previous` | Previous | none | lua |
| `goto.back` | Back | none | lua |
| `goto.forward` | Forward | none | lua |
| `search.results_next` | Results Next | none | lua |
| `search.results_previous` | Results Previous | none | lua |
| `palette.close` | Close | none | lua |
| `search.workspace` | Workspace | none | lua |
| `goto.line` | Go to Line | none | lua |

## selection-navigation

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `cursor.left` | Left | none | lua |
| `cursor.right` | Right | none | lua |
| `cursor.word_left` | Word Left | none | lua |
| `cursor.word_right` | Word Right | none | lua |
| `cursor.line_up` | Line Up | none | lua |
| `cursor.line_down` | Line Down | none | lua |
| `cursor.line_start` | Line Start | none | lua |
| `cursor.line_end` | Line End | none | lua |
| `cursor.page_up` | Page Up | none | lua |
| `cursor.page_down` | Page Down | none | lua |
| `cursor.document_start` | Document Start | none | lua |
| `cursor.document_end` | Document End | none | lua |
| `select.left` | Left | none | lua |
| `select.right` | Right | none | lua |
| `select.word_left` | Word Left | none | lua |
| `select.word_right` | Word Right | none | lua |
| `select.line_up` | Line Up | none | lua |
| `select.line_down` | Line Down | none | lua |
| `select.line_start` | Line Start | none | lua |
| `select.line_end` | Line End | none | lua |
| `select.page_up` | Page Up | none | lua |
| `select.page_down` | Page Down | none | lua |
| `select.document_start` | Document Start | none | lua |
| `select.document_end` | Document End | none | lua |
| `select.all` | All | none | lua |
| `select.add_next_occurrence` | Add Next Occurrence | none | lua |
| `select.add_cursor_up` | Add Cursor Up | none | lua |
| `select.add_cursor_down` | Add Cursor Down | none | lua |
| `select.split_into_lines` | Split Into Lines | none | lua |
| `select.to_matching_bracket` | To Matching Bracket | none | lua |
| `goto.matching_bracket` | Matching Bracket | none | lua |
| `view.reveal_caret` | Reveal Caret | none | lua |
| `view.center_caret` | Center Caret | none | lua |

## settings-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `settings.open` | Open Settings | none | lua |
| `settings.export_workspace` | Export Workspace | none | lua |

## shell-layout

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `pane.split_horizontal` | Split Editor Horizontally | none | lua |
| `pane.split_vertical` | Split Editor Vertically | none | lua |
| `pane.close` | Close Editor Pane | none | lua |
| `pane.next` | Next Editor Pane | none | lua |
| `pane.previous` | Previous Editor Pane | none | lua |
| `pane.focus_left` | Focus Editor Pane Left | none | — |
| `pane.focus_right` | Focus Editor Pane Right | none | — |
| `pane.focus_up` | Focus Editor Pane Up | none | — |
| `pane.focus_down` | Focus Editor Pane Down | none | — |
| `panel.toggle` | Toggle Sidebar | none | lua |
| `panel.focus` | Focus Sidebar | none | lua |
| `panel.toggle_focus` | Toggle Sidebar Focus | none | lua |
| `panel.shrink` | Shrink Sidebar | none | lua |
| `panel.grow` | Grow Sidebar | none | lua |
| `panel.show_files` | Show Files Sidebar | none | lua |
| `panel.show_git_status` | Show Git Sidebar | none | lua |
| `panel.show_search` | Show Search Sidebar | none | lua |
| `panel.next_provider` | Next Sidebar View | none | lua |
| `panel.previous_provider` | Previous Sidebar View | none | lua |
| `view.toggle_distraction_free` | Toggle Distraction Free | none | lua |

## style-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `style.define` | Define | none | lua, init.lua |

## tab-management

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `tab.close` | Close Tab | none | lua |
| `tab.close_others` | Close Other Tabs | none | lua |
| `tab.close_all` | Close All Tabs | none | lua |
| `tab.reopen_closed` | Reopen Closed | none | lua |
| `tab.next` | Next Tab | none | lua |
| `tab.previous` | Previous Tab | none | lua |
| `tab.move_left` | Move Left | none | lua |
| `tab.move_right` | Move Right | none | lua |

## text-input-commands

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `text.tab` | Insert Tab | none | lua |
| `text.newline` | Newline | none | lua |
| `text.delete_backward` | Delete Backward | none | lua |
| `text.delete_forward` | Delete Forward | none | lua |
| `text.delete_word_backward` | Delete Word Backward | none | lua |
| `text.delete_word_forward` | Delete Word Forward | none | lua |

## theme-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `theme.set` | Set Colors | none | lua, init.lua |

## tree-providers

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `tree.toggle_expanded` | Toggle Expanded | none | lua |
| `tree.select_next` | Select Next | none | lua |
| `tree.select_previous` | Select Previous | none | lua |
| `tree.activate` | Open Selected | none | lua |

## undo-redo-history

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `edit.undo` | Undo | none | lua |
| `edit.redo` | Redo | none | lua |

## viewport-wrap-scrollbar

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `view.toggle_word_wrap` | Toggle Word Wrap | none | lua |
| `view.toggle_line_numbers` | Toggle Line Numbers | none | lua |
