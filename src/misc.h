/* misc.h */

/*
 * This file is part of CliFM
 *
 * Copyright (C) 2016-2023, L. Abramovich <leo.clifm@outlook.com>
 * All rights reserved.

 * CliFM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * CliFM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
*/

#ifndef MISC_H
#define MISC_H

__BEGIN_DECLS

int  err(const int, const int, const char *, ...);
int  alias_import(char *);
void bonus_function(void);
int  confirm_sudo_cmd(char **);
int  create_usr_var(const char *);
int  expand_prompt_name(char *);
int  filter_function(char *);
void free_autocmds(void);
void free_prompts(void);
void free_software(void);
void free_stuff(void);
int  free_remotes(const int);
void free_tags(void);
void free_workspaces_filters(void);
char *gen_diff_str(const int);
char *get_newname(const char *, char *, int *);
void get_term_size(void);
int  handle_stdin(void);
void help_function(void);
int  is_blank_name(const char *s);
int  list_commands(void);
int  list_mountpoints(void);
int  new_instance(char *, int);
int  print_reload_msg(const char *, ...);
int  pin_directory(char *);
void print_tips(const int);
int  quick_help(char *);
void save_last_path(char *);
void set_eln_color(void);
void set_signals_to_ignore(void);
void set_term_title(char *);
void splash(void);
int  unpin_dir(void);
void version_function(void);

#ifdef LINUX_INOTIFY
void read_inotify(void);
void reset_inotify(void);
#elif defined(BSD_KQUEUE)
void read_kqueue(void);
#endif /* LINUX_INOTIFY */

void set_filter_type(const char);
/*void refresh_files_list(void); */

__END_DECLS

#endif /* MISC_H */
