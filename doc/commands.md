# Commands

Generated from the command catalog by `test_commands`. Do not
edit: change the command's registration instead, then regenerate
with `SSG_UPDATE_DOCS=1 ./build/test_commands`.

`init.lua` may call the commands marked `init.lua`; the rest are
available to the Lua API when a host grants them.

There are 191 commands.

## clipboard-register

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `clipboard.copy` | Copy | none | lua |
| `clipboard.cut` | Cut | none | lua |
| `clipboard.paste` | Paste | none | lua |

## diff-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `diff.next_hunk` | Next Hunk | none | lua |
| `diff.previous_hunk` | Previous Hunk | none | lua |
| `diff.open_file` | Open File | none | lua |

## draft-recovery

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `draft.diff` | Diff Draft Against Disk | none | lua |
| `draft.discard` | Discard Draft (Use Disk) | none | lua |
| `draft.dismiss` | Dismiss Draft Notice | none | lua |

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

## encoding-eol

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `file.reopen_with_encoding` | Reopen With Encoding | encoding | lua |
| `file.set_encoding` | Set Encoding | encoding | lua |
| `file.set_line_ending` | Set Line Ending | line ending | lua |
| `file.set_final_newline` | Set Final Newline | final newline | lua |

## external-modification-flow

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `external.reload` | Reload | none | lua |
| `external.keep_buffer` | Keep Buffer | none | lua |
| `external.open_diff` | Open Diff | none | lua |

## file-commands

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `workspace.open_directory` | Open Directory | none | lua |
| `file.new` | New File | none | lua |
| `file.open` | Open File | none | lua |
| `file.open_recent` | Open Recent | none | lua |
| `file.save` | Save File | none | lua |
| `file.save_all` | Save All Files | none | lua |
| `file.save_as` | Save File As | none | lua |
| `file.reload` | Reload File | none | lua |
| `file.rename` | Rename File | none | lua |
| `file.delete` | Delete File | none | lua |
| `file.new_directory` | New Directory | none | lua |
| `file.open_dropped_content` | Open Dropped Content | dropped content | — |

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
| `find.update_query` | Update Query | query | lua |
| `replace.update_replacement` | Update Replacement | query | lua |
| `replace.workspace_preview` | Workspace Preview | workspace replace | lua |
| `replace.workspace_apply` | Workspace Apply | workspace apply | lua |

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

## lsp-workspace-edits

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `rename.symbol` | Symbol | none | lua |

## prompt-status-surface

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `prompt.submit` | Submit Prompt | none | lua |
| `prompt.cancel` | Cancel | none | lua |
| `prompt.next` | Next | none | lua |
| `prompt.previous` | Previous | none | lua |
| `prompt.focus_next_control` | Focus Next Field | none | lua |
| `status.next` | Next | none | lua |
| `status.previous` | Previous | none | lua |
| `status.dismiss` | Dismiss | none | lua |
| `status.invoke_action` | Invoke Action | none | lua |
| `prompt.update_value` | Update Value | prompt value | lua |
| `prompt.focus_control` | Focus Field | prompt focus | lua |

## search-palette

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `palette.open` | Command Palette | none | lua |
| `file_finder.open` | Open | none | lua |
| `file_finder.toggle_gitignore` | Toggle Gitignore | none | lua |
| `palette.close` | Close | none | lua |
| `palette.next` | Next | none | lua |
| `palette.previous` | Previous | none | lua |
| `goto.back` | Back | none | lua |
| `goto.forward` | Forward | none | lua |
| `search.results_next` | Results Next | none | lua |
| `search.results_previous` | Results Previous | none | lua |
| `palette.execute` | Execute | palette selection | lua |
| `search.workspace` | Workspace | none | lua |
| `goto.file` | Go to File | none | lua |
| `goto.symbol` | Go to Symbol | none | lua |
| `goto.line` | Go to Line | none | lua |

## selection-navigation

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `cursor.set_position` | Set Position | selection | lua |
| `cursor.left` | Left | selection | lua |
| `cursor.right` | Right | selection | lua |
| `cursor.word_left` | Word Left | selection | lua |
| `cursor.word_right` | Word Right | selection | lua |
| `cursor.line_up` | Line Up | selection | lua |
| `cursor.line_down` | Line Down | selection | lua |
| `cursor.line_start` | Line Start | selection | lua |
| `cursor.line_end` | Line End | selection | lua |
| `cursor.page_up` | Page Up | selection | lua |
| `cursor.page_down` | Page Down | selection | lua |
| `cursor.document_start` | Document Start | selection | lua |
| `cursor.document_end` | Document End | selection | lua |
| `select.set_range` | Set Range | selection | lua |
| `select.set_ranges` | Set Ranges | selection | lua |
| `select.add_range` | Add Range | selection | lua |
| `select.left` | Left | selection | lua |
| `select.right` | Right | selection | lua |
| `select.word_left` | Word Left | selection | lua |
| `select.word_right` | Word Right | selection | lua |
| `select.line_up` | Line Up | selection | lua |
| `select.line_down` | Line Down | selection | lua |
| `select.line_start` | Line Start | selection | lua |
| `select.line_end` | Line End | selection | lua |
| `select.page_up` | Page Up | selection | lua |
| `select.page_down` | Page Down | selection | lua |
| `select.document_start` | Document Start | selection | lua |
| `select.document_end` | Document End | selection | lua |
| `select.all` | All | selection | lua |
| `select.add_next_occurrence` | Add Next Occurrence | selection | lua |
| `select.add_cursor_up` | Add Cursor Up | selection | lua |
| `select.add_cursor_down` | Add Cursor Down | selection | lua |
| `select.split_into_lines` | Split Into Lines | selection | lua |
| `select.to_matching_bracket` | To Matching Bracket | selection | lua |
| `goto.matching_bracket` | Matching Bracket | selection | lua |
| `select.word_at_position` | Word At Position | selection | lua |
| `view.reveal_caret` | Reveal Caret | selection | lua |
| `view.center_caret` | Center Caret | selection | lua |

## settings-model

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `settings.open` | Open Settings | none | lua |
| `settings.export_workspace` | Export Workspace | none | lua |
| `settings.import_workspace` | Import Workspace | none | lua |
| `settings.set` | Set | setting | lua |
| `settings.reset` | Reset | setting key | lua |
| `settings.reset_scope` | Reset Scope | setting scope | lua |

## shell-layout

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `pane.split_horizontal` | Split Horizontal | none | lua |
| `pane.split_vertical` | Split Vertical | none | lua |
| `pane.close` | Close | none | lua |
| `pane.next` | Next | none | lua |
| `pane.previous` | Previous | none | lua |
| `pane.focus_left` | Focus Left | none | lua |
| `pane.focus_right` | Focus Right | none | lua |
| `pane.focus_up` | Focus Up | none | lua |
| `pane.focus_down` | Focus Down | none | lua |
| `panel.toggle` | Toggle Sidebar | none | lua |
| `panel.focus` | Focus Sidebar | none | lua |
| `panel.show_files` | Show Files Sidebar | none | lua |
| `panel.show_git_status` | Show Git Sidebar | none | lua |
| `panel.next_provider` | Next Provider | none | lua |
| `panel.previous_provider` | Previous Provider | none | lua |
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
| `tab.activate` | Activate | none | lua |
| `tab.move_left` | Move Left | none | lua |
| `tab.move_right` | Move Right | none | lua |

## text-input-commands

| Command | Summary | Arguments | Surfaces |
|---|---|---|---|
| `text.insert` | Insert | text | lua |
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
| `tree.invoke_node_command` | Invoke Node Command | none | lua |
| `tree.select` | Select | tree node | lua |
| `tree.scroll` | Scroll | scroll lines | lua |
| `tree.scroll_to_fraction` | Scroll To Fraction | scroll fraction | lua |

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
| `view.scroll_lines` | Scroll Lines | scroll lines | lua |
| `view.scroll_pages` | Scroll Pages | scroll pages | lua |
| `view.scroll_to_fraction` | Scroll To Fraction | scroll fraction | lua |
