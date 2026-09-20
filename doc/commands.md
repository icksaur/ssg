# Commands

Generated from the command catalog by `test_commands`. Do not
edit: change the command's registration instead, then regenerate
with `SSG_UPDATE_DOCS=1 ./build/test_commands`.

SSG has no scripting surface. The command catalog below documents the
built-in command ids; the Surfaces column remains for format stability and uses `—`.

There are 162 commands.

## clipboard-register

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `clipboard.copy` | Copy | none | — |
| `clipboard.cut` | Cut | none | — |
| `clipboard.paste` | Paste | none | — |

## edit-command-suite

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `edit.indent` | Indent | none | — |
| `edit.outdent` | Outdent | none | — |
| `edit.duplicate_line` | Duplicate Line | none | — |
| `edit.move_line_up` | Move Line Up | none | — |
| `edit.move_line_down` | Move Line Down | none | — |
| `edit.delete_line` | Delete Line | none | — |
| `edit.join_lines` | Join Lines | none | — |
| `edit.uppercase` | Uppercase | none | — |
| `edit.lowercase` | Lowercase | none | — |
| `edit.swap_case` | Swap Case | none | — |
| `edit.sort_lines` | Sort Lines | none | — |
| `edit.transpose` | Transpose | none | — |
| `edit.toggle_comment` | Toggle Comment | none | — |

## external-modification-flow

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `external.reload` | Reload | none | — |
| `external.keep_buffer` | Keep Buffer | none | — |
| `external.open_diff` | Open Diff | none | — |
| `external.select_next` | Select Next External Change | none | — |
| `external.select_previous` | Select Previous External Change | none | — |
| `external.focus` | Focus External Change Bar | none | — |
| `external.focus_return` | Leave External Change Bar | none | — |

## file-commands

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `workspace.open_directory` | Open Directory | none | — |
| `file.new` | New File | none | — |
| `file.open` | Open File | none | — |
| `file.save` | Save File | none | — |
| `file.save_all` | Save All Files | none | — |
| `file.save_as` | Save File As | none | — |
| `file.reload` | Reload File | none | — |
| `file.rename` | Rename File | none | — |
| `file.delete` | Delete File | none | — |
| `file.new_directory` | New Directory | none | — |

## find-replace

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `find.open` | Find | none | — |
| `find.word_under_cursor` | Find Word Under Cursor | none | — |
| `find.close` | Close | none | — |
| `find.next` | Next | none | — |
| `find.previous` | Previous | none | — |
| `find.toggle_case` | Toggle Case | none | — |
| `find.toggle_whole_word` | Toggle Whole Word | none | — |
| `find.toggle_regex` | Toggle Regex | none | — |
| `find.toggle_selection` | Toggle Selection | none | — |
| `replace.open` | Replace | none | — |
| `replace.current` | Current | none | — |
| `replace.all` | All | none | — |

## follow-edits

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `follow_edits.resume` | Resume | none | — |
| `follow_edits.pause` | Pause | none | — |
| `follow_edits.toggle` | Toggle | none | — |

## help-system

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `help.open` | Open Help | none | — |

## keymap-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `keymap.bind` | Bind | none | — |
| `keymap.unbind` | Unbind | none | — |

## lsp-language-features

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `goto.definition` | Go to Definition | none | — |
| `goto.reference` | Reference | none | — |
| `completion.open` | Open | none | — |
| `completion.next` | Next | none | — |
| `completion.previous` | Previous | none | — |
| `completion.accept` | Accept | none | — |
| `completion.dismiss` | Dismiss | none | — |
| `hover.show` | Show | none | — |
| `hover.dismiss` | Dismiss | none | — |

## prompt-status-surface

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `prompt.submit` | Submit Prompt | none | — |
| `prompt.cancel` | Cancel | none | — |
| `prompt.next` | Next | none | — |
| `prompt.previous` | Previous | none | — |
| `prompt.focus_next_control` | Focus Next Field | none | — |

## search-palette

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `palette.open` | Command Palette | none | — |
| `file_finder.open` | Open | none | — |
| `file_finder.toggle_gitignore` | Toggle Gitignore | none | — |
| `palette.next` | Next | none | — |
| `palette.previous` | Previous | none | — |
| `goto.back` | Back | none | — |
| `goto.forward` | Forward | none | — |
| `search.results_next` | Results Next | none | — |
| `search.results_previous` | Results Previous | none | — |
| `palette.close` | Close | none | — |
| `search.workspace` | Workspace | none | — |
| `goto.line` | Go to Line | none | — |

## selection-navigation

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `cursor.left` | Left | none | — |
| `cursor.right` | Right | none | — |
| `cursor.word_left` | Word Left | none | — |
| `cursor.word_right` | Word Right | none | — |
| `cursor.line_up` | Line Up | none | — |
| `cursor.line_down` | Line Down | none | — |
| `cursor.line_start` | Line Start | none | — |
| `cursor.line_end` | Line End | none | — |
| `cursor.page_up` | Page Up | none | — |
| `cursor.page_down` | Page Down | none | — |
| `cursor.document_start` | Document Start | none | — |
| `cursor.document_end` | Document End | none | — |
| `select.left` | Left | none | — |
| `select.right` | Right | none | — |
| `select.word_left` | Word Left | none | — |
| `select.word_right` | Word Right | none | — |
| `select.line_up` | Line Up | none | — |
| `select.line_down` | Line Down | none | — |
| `select.line_start` | Line Start | none | — |
| `select.line_end` | Line End | none | — |
| `select.page_up` | Page Up | none | — |
| `select.page_down` | Page Down | none | — |
| `select.document_start` | Document Start | none | — |
| `select.document_end` | Document End | none | — |
| `select.all` | All | none | — |
| `select.add_next_occurrence` | Add Next Occurrence | none | — |
| `select.add_cursor_up` | Add Cursor Up | none | — |
| `select.add_cursor_down` | Add Cursor Down | none | — |
| `select.split_into_lines` | Split Into Lines | none | — |
| `select.to_matching_bracket` | To Matching Bracket | none | — |
| `goto.matching_bracket` | Matching Bracket | none | — |
| `view.reveal_caret` | Reveal Caret | none | — |
| `view.center_caret` | Center Caret | none | — |

## settings-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `settings.open` | Open Settings | none | — |
| `settings.export_workspace` | Export Workspace | none | — |

## shell-layout

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `pane.split_horizontal` | Split Editor Horizontally | none | — |
| `pane.split_vertical` | Split Editor Vertically | none | — |
| `pane.close` | Close Editor Pane | none | — |
| `pane.next` | Next Editor Pane | none | — |
| `pane.previous` | Previous Editor Pane | none | — |
| `pane.focus_left` | Focus Editor Pane Left | none | — |
| `pane.focus_right` | Focus Editor Pane Right | none | — |
| `pane.focus_up` | Focus Editor Pane Up | none | — |
| `pane.focus_down` | Focus Editor Pane Down | none | — |
| `panel.toggle` | Toggle Sidebar | none | — |
| `panel.focus` | Focus Sidebar | none | — |
| `panel.toggle_focus` | Toggle Sidebar Focus | none | — |
| `panel.shrink` | Shrink Sidebar | none | — |
| `panel.grow` | Grow Sidebar | none | — |
| `panel.show_files` | Show Files Sidebar | none | — |
| `panel.show_git_status` | Show Git Sidebar | none | — |
| `panel.show_search` | Show Search Sidebar | none | — |
| `panel.next_provider` | Next Sidebar View | none | — |
| `panel.previous_provider` | Previous Sidebar View | none | — |
| `view.toggle_distraction_free` | Toggle Distraction Free | none | — |

## style-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `style.define` | Define | none | — |

## tab-management

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `tab.close` | Close Tab | none | — |
| `tab.close_others` | Close Other Tabs | none | — |
| `tab.close_all` | Close All Tabs | none | — |
| `tab.reopen_closed` | Reopen Closed | none | — |
| `tab.next` | Next Tab | none | — |
| `tab.previous` | Previous Tab | none | — |
| `tab.move_left` | Move Left | none | — |
| `tab.move_right` | Move Right | none | — |

## text-input-commands

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `text.tab` | Insert Tab | none | — |
| `text.newline` | Newline | none | — |
| `text.delete_backward` | Delete Backward | none | — |
| `text.delete_forward` | Delete Forward | none | — |
| `text.delete_word_backward` | Delete Word Backward | none | — |
| `text.delete_word_forward` | Delete Word Forward | none | — |

## theme-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `theme.set` | Set Colors | none | — |

## tree-providers

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `tree.toggle_expanded` | Toggle Expanded | none | — |
| `tree.select_next` | Select Next | none | — |
| `tree.select_previous` | Select Previous | none | — |
| `tree.activate` | Open Selected | none | — |

## undo-redo-history

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `edit.undo` | Undo | none | — |
| `edit.redo` | Redo | none | — |

## viewport-wrap-scrollbar

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `view.toggle_word_wrap` | Toggle Word Wrap | none | — |
| `view.toggle_line_numbers` | Toggle Line Numbers | none | — |
