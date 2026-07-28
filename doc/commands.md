# Commands

Generated from the command catalog in `src/Commands.cpp` by
`ssg_command_docs`. Do not edit: change the catalog instead.

`init.lua` may call the commands marked `init.lua`; the rest are
available to the Lua API when a host grants them
(see `doc/spec-commands.md`).

There are 182 commands.

## clipboard-register

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `clipboard.copy` | Copy | none | keymap, palette, lua |
| `clipboard.cut` | Cut | none | keymap, palette, lua |
| `clipboard.paste` | Paste | none | keymap, palette, lua |

## diff-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `diff.next_hunk` | Next Hunk | none | keymap, palette, lua |
| `diff.previous_hunk` | Previous Hunk | none | keymap, palette, lua |
| `diff.open_file` | Open File | none | keymap, palette, lua |

## edit-command-suite

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `edit.indent` | Indent | none | keymap, palette, lua |
| `edit.outdent` | Outdent | none | keymap, palette, lua |
| `edit.duplicate_line` | Duplicate Line | none | keymap, palette, lua |
| `edit.move_line_up` | Move Line Up | none | keymap, palette, lua |
| `edit.move_line_down` | Move Line Down | none | keymap, palette, lua |
| `edit.delete_line` | Delete Line | none | keymap, palette, lua |
| `edit.join_lines` | Join Lines | none | keymap, palette, lua |
| `edit.uppercase` | Uppercase | none | keymap, palette, lua |
| `edit.lowercase` | Lowercase | none | keymap, palette, lua |
| `edit.swap_case` | Swap Case | none | keymap, palette, lua |
| `edit.sort_lines` | Sort Lines | none | keymap, palette, lua |
| `edit.transpose` | Transpose | none | keymap, palette, lua |
| `edit.toggle_comment` | Toggle Comment | none | keymap, palette, lua |

## encoding-eol

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `file.reopen_with_encoding` | Reopen With Encoding | encoding | keymap, palette, lua |
| `file.set_encoding` | Set Encoding | encoding | keymap, palette, lua |
| `file.set_line_ending` | Set Line Ending | line ending | keymap, palette, lua |
| `file.set_final_newline` | Set Final Newline | final newline | keymap, palette, lua |

## external-modification-flow

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `external.reload` | Reload | none | keymap, palette, lua |
| `external.keep_buffer` | Keep Buffer | none | keymap, palette, lua |
| `external.open_diff` | Open Diff | none | keymap, palette, lua |

## file-commands

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `workspace.open_directory` | Open Directory | none | keymap, palette, lua |
| `file.new` | New File | none | keymap, palette, lua |
| `file.open` | Open File | none | keymap, palette, lua |
| `file.open_recent` | Open Recent | none | keymap, palette, lua |
| `file.open_dropped_content` | Open Dropped Content | dropped content | — |
| `file.save` | Save File | none | keymap, palette, lua |
| `file.save_all` | Save All Files | none | keymap, palette, lua |
| `file.save_as` | Save File As | none | keymap, palette, lua |
| `file.reload` | Reload File | none | keymap, palette, lua |
| `file.rename` | Rename File | none | keymap, palette, lua |
| `file.delete` | Delete File | none | keymap, palette, lua |
| `file.new_directory` | New Directory | none | keymap, palette, lua |

## find-replace

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `find.open` | Find | none | keymap, palette, lua |
| `find.close` | Close | none | keymap, palette, lua |
| `find.next` | Next | none | keymap, palette, lua |
| `find.previous` | Previous | none | keymap, palette, lua |
| `find.update_query` | Update Query | query | lua |
| `find.toggle_case` | Toggle Case | none | keymap, palette, lua |
| `find.toggle_whole_word` | Toggle Whole Word | none | keymap, palette, lua |
| `find.toggle_regex` | Toggle Regex | none | keymap, palette, lua |
| `find.toggle_selection` | Toggle Selection | none | keymap, palette, lua |
| `replace.open` | Replace | none | keymap, palette, lua |
| `replace.update_replacement` | Update Replacement | query | lua |
| `replace.current` | Current | none | keymap, palette, lua |
| `replace.all` | All | none | keymap, palette, lua |
| `replace.workspace_preview` | Workspace Preview | workspace replace | keymap, palette, lua |
| `replace.workspace_apply` | Workspace Apply | workspace apply | keymap, palette, lua |

## follow-edits

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `follow_edits.resume` | Resume | none | keymap, palette, lua |
| `follow_edits.pause` | Pause | none | keymap, palette, lua |
| `follow_edits.toggle` | Toggle | none | keymap, palette, lua |

## keymap-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `keymap.bind` | Bind | none | lua, init.lua |
| `keymap.unbind` | Unbind | none | lua, init.lua |

## lsp-language-features

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `goto.definition` | Go to Definition | none | keymap, palette, lua |
| `goto.reference` | Reference | none | keymap, palette, lua |
| `completion.open` | Open | none | keymap, palette, lua |
| `completion.next` | Next | none | keymap, palette, lua |
| `completion.previous` | Previous | none | keymap, palette, lua |
| `completion.accept` | Accept | none | keymap, palette, lua |
| `completion.dismiss` | Dismiss | none | keymap, palette, lua |
| `hover.show` | Show | none | keymap, palette, lua |
| `hover.dismiss` | Dismiss | none | keymap, palette, lua |

## lsp-workspace-edits

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `rename.symbol` | Symbol | none | keymap, palette, lua |

## prompt-status-surface

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `prompt.submit` | Submit Prompt | none | keymap, palette, lua |
| `prompt.cancel` | Cancel | none | keymap, palette, lua |
| `prompt.next` | Next | none | keymap, palette, lua |
| `prompt.previous` | Previous | none | keymap, palette, lua |
| `prompt.update_value` | Update Value | prompt value | lua |
| `status.next` | Next | none | keymap, palette, lua |
| `status.previous` | Previous | none | keymap, palette, lua |
| `status.dismiss` | Dismiss | none | keymap, palette, lua |
| `status.invoke_action` | Invoke Action | none | keymap, palette, lua |

## search-palette

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `palette.open` | Command Palette | none | keymap, palette, lua |
| `file_finder.open` | Open | none | keymap, palette, lua |
| `file_finder.toggle_gitignore` | Toggle Gitignore | none | keymap, palette, lua |
| `palette.close` | Close | none | keymap, palette, lua |
| `palette.next` | Next | none | keymap, palette, lua |
| `palette.previous` | Previous | none | keymap, palette, lua |
| `palette.execute` | Execute | palette selection | keymap, palette, lua |
| `goto.file` | Go to File | none | keymap, palette, lua |
| `goto.line` | Go to Line | none | keymap, palette, lua |
| `goto.symbol` | Go to Symbol | none | keymap, palette, lua |
| `goto.back` | Back | none | keymap, palette, lua |
| `goto.forward` | Forward | none | keymap, palette, lua |
| `search.workspace` | Workspace | none | keymap, palette, lua |
| `search.results_next` | Results Next | none | keymap, palette, lua |
| `search.results_previous` | Results Previous | none | keymap, palette, lua |

## selection-navigation

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `cursor.set_position` | Set Position | selection | keymap, palette, lua |
| `cursor.left` | Left | selection | keymap, palette, lua |
| `cursor.right` | Right | selection | keymap, palette, lua |
| `cursor.word_left` | Word Left | selection | keymap, palette, lua |
| `cursor.word_right` | Word Right | selection | keymap, palette, lua |
| `cursor.line_up` | Line Up | selection | keymap, palette, lua |
| `cursor.line_down` | Line Down | selection | keymap, palette, lua |
| `cursor.line_start` | Line Start | selection | keymap, palette, lua |
| `cursor.line_end` | Line End | selection | keymap, palette, lua |
| `cursor.page_up` | Page Up | selection | keymap, palette, lua |
| `cursor.page_down` | Page Down | selection | keymap, palette, lua |
| `cursor.document_start` | Document Start | selection | keymap, palette, lua |
| `cursor.document_end` | Document End | selection | keymap, palette, lua |
| `select.set_range` | Set Range | selection | keymap, palette, lua |
| `select.add_range` | Add Range | selection | keymap, palette, lua |
| `select.left` | Left | selection | keymap, palette, lua |
| `select.right` | Right | selection | keymap, palette, lua |
| `select.word_left` | Word Left | selection | keymap, palette, lua |
| `select.word_right` | Word Right | selection | keymap, palette, lua |
| `select.line_up` | Line Up | selection | keymap, palette, lua |
| `select.line_down` | Line Down | selection | keymap, palette, lua |
| `select.line_start` | Line Start | selection | keymap, palette, lua |
| `select.line_end` | Line End | selection | keymap, palette, lua |
| `select.page_up` | Page Up | selection | keymap, palette, lua |
| `select.page_down` | Page Down | selection | keymap, palette, lua |
| `select.document_start` | Document Start | selection | keymap, palette, lua |
| `select.document_end` | Document End | selection | keymap, palette, lua |
| `select.all` | All | selection | keymap, palette, lua |
| `select.add_next_occurrence` | Add Next Occurrence | selection | keymap, palette, lua |
| `select.add_cursor_up` | Add Cursor Up | selection | keymap, palette, lua |
| `select.add_cursor_down` | Add Cursor Down | selection | keymap, palette, lua |
| `select.split_into_lines` | Split Into Lines | selection | keymap, palette, lua |
| `select.to_matching_bracket` | To Matching Bracket | selection | keymap, palette, lua |
| `view.reveal_caret` | Reveal Caret | selection | keymap, palette, lua |
| `view.center_caret` | Center Caret | selection | keymap, palette, lua |
| `goto.matching_bracket` | Matching Bracket | selection | keymap, palette, lua |

## settings-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `settings.open` | Open Settings | none | keymap, palette, lua |
| `settings.set` | Set | setting | keymap, palette, lua |
| `settings.reset` | Reset | setting key | keymap, palette, lua |
| `settings.reset_scope` | Reset Scope | setting scope | keymap, palette, lua |
| `settings.export_workspace` | Export Workspace | none | keymap, palette, lua |
| `settings.import_workspace` | Import Workspace | none | keymap, palette, lua |

## shell-layout

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `pane.split_horizontal` | Split Horizontal | none | keymap, palette, lua |
| `pane.split_vertical` | Split Vertical | none | keymap, palette, lua |
| `pane.close` | Close | none | keymap, palette, lua |
| `pane.next` | Next | none | keymap, palette, lua |
| `pane.previous` | Previous | none | keymap, palette, lua |
| `pane.focus_left` | Focus Left | none | keymap, palette, lua |
| `pane.focus_right` | Focus Right | none | keymap, palette, lua |
| `pane.focus_up` | Focus Up | none | keymap, palette, lua |
| `pane.focus_down` | Focus Down | none | keymap, palette, lua |
| `panel.toggle` | Toggle Sidebar | none | keymap, palette, lua |
| `panel.focus` | Focus Sidebar | none | keymap, palette, lua |
| `panel.show_files` | Show Files Sidebar | none | keymap, palette, lua |
| `panel.show_git_status` | Show Git Sidebar | none | keymap, palette, lua |
| `panel.next_provider` | Next Provider | none | keymap, palette, lua |
| `panel.previous_provider` | Previous Provider | none | keymap, palette, lua |
| `view.toggle_distraction_free` | Toggle Distraction Free | none | keymap, palette, lua |

## style-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `style.define` | Define | none | lua, init.lua |

## tab-management

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `tab.close` | Close Tab | none | keymap, palette, lua |
| `tab.close_others` | Close Other Tabs | none | keymap, palette, lua |
| `tab.close_all` | Close All Tabs | none | keymap, palette, lua |
| `tab.reopen_closed` | Reopen Closed | none | keymap, palette, lua |
| `tab.next` | Next Tab | none | keymap, palette, lua |
| `tab.previous` | Previous Tab | none | keymap, palette, lua |
| `tab.activate` | Activate | none | keymap, palette, lua |
| `tab.move_left` | Move Left | none | keymap, palette, lua |
| `tab.move_right` | Move Right | none | keymap, palette, lua |

## text-input-commands

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `text.insert` | Insert | text | keymap, palette, lua |
| `text.newline` | Newline | text | keymap, palette, lua |
| `text.delete_backward` | Delete Backward | text | keymap, palette, lua |
| `text.delete_forward` | Delete Forward | text | keymap, palette, lua |
| `text.delete_word_backward` | Delete Word Backward | text | keymap, palette, lua |
| `text.delete_word_forward` | Delete Word Forward | text | keymap, palette, lua |

## theme-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `theme.define` | Define | none | keymap, palette, lua, init.lua |
| `theme.background` | Background | none | keymap, palette, lua, init.lua |

## tree-providers

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `tree.toggle_expanded` | Toggle Expanded | none | keymap, palette, lua |
| `tree.invoke_node_command` | Invoke Node Command | none | keymap, palette, lua |
| `tree.select` | Select | tree node | lua |
| `tree.select_next` | Select Next | none | keymap, palette, lua |
| `tree.select_previous` | Select Previous | none | keymap, palette, lua |
| `tree.activate` | Open Selected | none | keymap, palette, lua |
| `tree.scroll` | Scroll | scroll lines | lua |
| `tree.scroll_to_fraction` | Scroll To Fraction | scroll fraction | lua |

## undo-redo-history

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `edit.undo` | Undo | none | keymap, palette, lua |
| `edit.redo` | Redo | none | keymap, palette, lua |

## viewport-wrap-scrollbar

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `view.toggle_word_wrap` | Toggle Word Wrap | none | keymap, palette, lua |
| `view.scroll_lines` | Scroll Lines | scroll lines | keymap, palette, lua |
| `view.scroll_pages` | Scroll Pages | scroll pages | keymap, palette, lua |
| `view.scroll_to_fraction` | Scroll To Fraction | scroll fraction | keymap, palette, lua |
