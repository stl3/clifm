/* trash.c -- functions to manage the trash system */

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

#ifndef _NO_TRASH

#include "helpers.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "aux.h"
#include "checks.h"
#include "colors.h"
#include "exec.h"
#include "misc.h"
#include "navigation.h"
#include "readline.h"
#include "sort.h"
#include "trash.h"
#include "listing.h"

static size_t
count_trashed_files(void)
{
	size_t n = 0;
	if (trash_ok == 1 && trash_files_dir) {
		n = (size_t)count_dir(trash_files_dir, NO_CPOP);
		if (n <= 2)
			n = 0;
		else
			n -= 2;
	}

	return n;
}

/* Recursively check directory permissions (write and execute). Returns
 * zero if OK, and one if at least one subdirectory does not have
 * write/execute permissions */
static int
recur_perm_check(const char *dirname)
{
	DIR *dir;
	struct dirent *ent;
#if !defined(_DIRENT_HAVE_D_TYPE)
	struct stat attr;
#endif /* !_DIRENT_HAVE_D_TYPE */

	if (!(dir = opendir(dirname)))
		return EXIT_FAILURE;

	while ((ent = readdir(dir)) != NULL) {
#if !defined(_DIRENT_HAVE_D_TYPE)
		if (lstat(ent->d_name, &attr) == -1)
			continue;
		if (S_ISDIR(attr.st_mode)) {
#else
		if (ent->d_type == DT_DIR) {
#endif /* !_DIRENT_HAVE_D_TYPE */
			char dirpath[PATH_MAX] = "";

			if (*ent->d_name == '.' && (!ent->d_name[1]
			|| (ent->d_name[1] == '.' && !ent->d_name[2])))
				continue;

			snprintf(dirpath, PATH_MAX, "%s/%s", dirname, ent->d_name);

			if (access(dirpath, W_OK | X_OK) != 0) {
				/* recur_perm_error_flag needs to be a global variable.
				  * Otherwise, since this function calls itself
				  * recursivelly, the flag would be reset upon every
				  * new call, without preserving the error code, which
				  * is what the flag is aimed to do. On the other side,
				  * if I use a local static variable for this flag, it
				  * will never drop the error value, and all subsequent
				  * calls to the function will allways return error
				  * (even if there's no actual error) */
				recur_perm_error_flag = 1;
				xerror(_("%s: Permission denied\n"), dirpath);
			}

			recur_perm_check(dirpath);
		}
	}

	closedir(dir);

	if (recur_perm_error_flag)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

/* Check whether the current user has enough permissions (write, execute)
 * to modify the contents of the parent directory of 'file'. 'file' needs
 * to be an absolute path. Returns zero if yes and one if no. Useful to
 * know if a file can be removed from or copied into the parent. In case
 * FILE is a directory, the function checks all its subdirectories for
 * appropriate permissions, including the immutable bit */
static int
wx_parent_check(char *file)
{
	struct stat attr;
	int exit_status = -1, ret = -1;
	size_t file_len = strlen(file);

	if (file_len > 0 && file[file_len - 1] == '/')
		file[file_len - 1] = '\0';

	if (lstat(file, &attr) == -1) {
		xerror("%s: %s\n", file, strerror(errno));
		return EXIT_FAILURE;
	}

	char *parent = strbfrlst(file, '/');
	if (!parent) {
		/* strbfrlst() will return NULL if file's parent is root (/),
		 * simply because in this case there's nothing before the last
		 * slash. So, check if file's parent dir is root */
		if (file[0] == '/' && strcntchr(file + 1, '/') == -1) {
			parent = (char *)xnmalloc(2, sizeof(char));
			parent[0] = '/';
			parent[1] = '\0';
		} else {
			xerror(_("trash: %s: Error getting parent directory\n"), file);
			return EXIT_FAILURE;
		}
	}

	switch (attr.st_mode & S_IFMT) {
	case S_IFDIR:
		ret = check_immutable_bit(file);

		if (ret == -1) {
			/* Error message is printed by check_immutable_bit() itself */
			exit_status = EXIT_FAILURE;
		} else if (ret == 1) {
			xerror(_("%s: Directory is immutable\n"), file);
			exit_status = EXIT_FAILURE;
		} else if (access(parent, W_OK | X_OK) == 0) {
		/* Check the parent for appropriate permissions */
			filesn_t files_n = count_dir(parent, NO_CPOP);

			if (files_n > 2) {
				/* I manually check here subdir because recur_perm_check()
				 * will only check the contents of subdir, but not subdir
				 * itself */
				/* If the parent is ok and not empty, check subdir */
				if (access(file, W_OK | X_OK) == 0) {
					/* If subdir is ok and not empty, recusivelly check
					 * subdir */
					files_n = count_dir(file, NO_CPOP);

					if (files_n > 2) {
						/* Reset the recur_perm_check() error flag. See
						 * the note in the function block. */
						recur_perm_error_flag = 0;

						if (recur_perm_check(file) == 0) {
							exit_status = EXIT_SUCCESS;
						} else {
							/* recur_perm_check itself will print the
							 * error messages */
							exit_status = EXIT_FAILURE;
						}
					} else { /* Subdir is ok and empty */
						exit_status = EXIT_SUCCESS;
					}
				} else { /* No permission for subdir */
					xerror(_("%s: Permission denied\n"), file);
					exit_status = EXIT_FAILURE;
				}
			} else {
				exit_status = EXIT_SUCCESS;
			}
		} else { /* No permission for parent */
			xerror(_("%s: Permission denied\n"), parent);
			exit_status = EXIT_FAILURE;
		}
		break;

	case S_IFREG:
		ret = check_immutable_bit(file);

		if (ret == -1) {
			/* Error message is printed by check_immutable_bit() itself */
			exit_status = EXIT_FAILURE;
		} else if (ret == 1) {
			xerror(_("%s: File is immutable\n"), file);
			exit_status = EXIT_FAILURE;
		} else {
			if (parent) {
				if (access(parent, W_OK | X_OK) == 0) {
					exit_status = EXIT_SUCCESS;
				} else {
					xerror(_("%s: Permission denied\n"), parent);
					exit_status = EXIT_FAILURE;
				}
			}
		}

		break;

#ifdef SOLARIS_DOORS
	case S_IFDOOR: /* fallthrough */
	case S_IFPORT: /* fallthrough */
#endif /* SOLARIS_DOORS */
	case S_IFSOCK: /* fallthrough */
	case S_IFIFO: /* fallthrough */
	case S_IFLNK:
		/* Symlinks, sockets and pipes do not support immutable bit */
		if (parent) {
			if (access(parent, W_OK | X_OK) == 0) {
				exit_status = EXIT_SUCCESS;
			} else {
				xerror(_("%s: Permission denied\n"), parent);
				exit_status = EXIT_FAILURE;
			}
		}
		break;

	/* DO NOT TRASH BLOCK AND CHAR DEVICES */
	default:
		xerror(_("trash: %s (%s): Unsupported file type\n"),
			file, S_ISBLK(attr.st_mode) ? "Block device"
		    : (S_ISCHR(attr.st_mode) ? _("Character device")
		    : _("Unknown file type")));
		exit_status = EXIT_FAILURE;
		break;
	}

	if (parent)
		free(parent);

	return exit_status;
}

static int
trash_clear(void)
{
	struct dirent **trash_files = (struct dirent **)NULL;
	int files_n = -1, exit_status = EXIT_SUCCESS;

	if (xchdir(trash_files_dir, NO_TITLE) == -1) {
		err(0, NOPRINT_PROMPT, "trash: %s: %s\n",
			trash_files_dir, strerror(errno));
		return EXIT_FAILURE;
	}

	files_n = scandir(trash_files_dir, &trash_files, skip_files, xalphasort);

	if (!files_n) {
		puts(_("trash: No trashed files"));
		return EXIT_SUCCESS;
	}

	int ret = EXIT_SUCCESS;
	size_t i;
	for (i = 0; i < (size_t)files_n; i++) {
		size_t len = strlen(trash_files[i]->d_name) + 11;
		char *info_file = (char *)xnmalloc(len, sizeof(char));
		snprintf(info_file, len, "%s.trashinfo", trash_files[i]->d_name);

		char *file1 = (char *)NULL;
		len = strlen(trash_files_dir) + strlen(trash_files[i]->d_name) + 2;
		file1 = (char *)xnmalloc(len, sizeof(char));
		snprintf(file1, len, "%s/%s", trash_files_dir, trash_files[i]->d_name);

		char *file2 = (char *)NULL;
		len = strlen(trash_info_dir) + strlen(info_file) + 2;
		file2 = (char *)xnmalloc(len, sizeof(char));
		snprintf(file2, len, "%s/%s", trash_info_dir, info_file);

		char *tmp_cmd[] = {"rm", "-rf", "--", file1, file2, NULL};
		ret = launch_execv(tmp_cmd, FOREGROUND, E_NOFLAG);

		free(file1);
		free(file2);

		if (ret != EXIT_SUCCESS) {
			xerror(_("trash: %s: Error removing trashed file\n"),
				trash_files[i]->d_name);
			exit_status = ret;
			/* If there is at least one error, return error */
		}

		free(info_file);
		free(trash_files[i]);
	}

	free(trash_files);

	if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
		err(0, NOPRINT_PROMPT, "trash: '%s': %s\n",
			workspaces[cur_ws].path, strerror(errno));
		return EXIT_FAILURE;
	}

	if (ret == EXIT_SUCCESS) {
		if (conf.autols == 1)
			reload_dirlist();
		print_reload_msg(_("Trash can emptied\n"));
	}

	return exit_status;
}

static int
del_trash_file_and_exit(char **file_suffix, char **info_file)
{
	size_t len = strlen(trash_files_dir) + strlen(*file_suffix) + 2;
	char *trash_file = (char *)xnmalloc(len, sizeof(char));
	snprintf(trash_file, len, "%s/%s", trash_files_dir, *file_suffix);

	char *tmp_cmd[] = {"rm", "-rf", "--", trash_file, NULL};
	int ret = launch_execv(tmp_cmd, FOREGROUND, E_NOFLAG);
	free(trash_file);

	if (ret != EXIT_SUCCESS) {
		xerror(_("trash: %s/%s: Failed removing trash file\nTry "
			"removing it manually\n"), trash_files_dir, *file_suffix);
	}

	free(*file_suffix);
	free(*info_file);

	return ret;
}

static int
trash_file(const char *suffix, const struct tm *tm, char *file)
{
	struct stat attr;
	if (lstat(file, &attr) == -1) {
		xerror("trash: %s: %s\n", file, strerror(errno));
		return errno;
	}

	char *tmpfile = file;
	char full_path[PATH_MAX];

	if (*file != '/') { /* If relative path, make it absolute. */
		if (!workspaces[cur_ws].path)
			return EXIT_FAILURE;

		snprintf(full_path, sizeof(full_path), "%s/%s",
			workspaces[cur_ws].path, file);
		tmpfile = full_path;
	}

	/* Check whether the user has enough permissions to remove file */
	if (wx_parent_check(tmpfile) != 0)
		return EXIT_FAILURE;

	int ret = -1;

	/* Create the trashed file name: orig_filename.suffix, where SUFFIX is
	 * current date and time. */
	char *filename = strrchr(tmpfile, '/');
	if (!filename || !*(++filename)) {
		xerror(_("trash: %s: Error getting file name\n"), file);
		return EXIT_FAILURE;
	}

	/* If the length of the trashed file name (orig_filename.suffix) is
	 * longer than NAME_MAX (255), trim the original filename, so that
	 * (original_filename_len + 1 (dot) + suffix_len) won't be longer
	 * than NAME_MAX. */
	size_t filename_len = strlen(filename);
	size_t suffix_len = strlen(suffix);
	int size = (int)(filename_len + suffix_len + 11) - NAME_MAX;
	/* len = filename.suffix.trashinfo */

	if (size > 0) {
		/* THIS IS NOT UNICODE AWARE */
		/* If SIZE is a positive value, that is, the trashed file name
		 * exceeds NAME_MAX by SIZE bytes, reduce the original file name
		 * SIZE bytes. Terminate the original file name (FILENAME) with
		 * a tilde (~), to let the user know it is trimmed. */
		filename[filename_len - (size_t)size - 1] = '~';
		filename[filename_len - (size_t)size] = '\0';
	}

	size_t len = filename_len + suffix_len + 2;
	char *file_suffix = (char *)xnmalloc(len, sizeof(char));
	snprintf(file_suffix, len, "%s.%s", filename, suffix);

	/* Move the original file into the trash directory. */
	/* NOTE: It is guaranteed (by check_trash_file()) that FILE does not
	 * end with a slash. */
	len = strlen(trash_files_dir) + strlen(file_suffix) + 2;
	char *dest = (char *)xnmalloc(len, sizeof(char));
	snprintf(dest, len, "%s/%s", trash_files_dir, file_suffix);

	int mvcmd = 0;
	ret = renameat(XAT_FDCWD, file, XAT_FDCWD, dest);
	if (ret != EXIT_SUCCESS && errno == EXDEV) {
		/* Destination file is on a different file system, which is why
		 * renameat(2) doesn't work: let's try with mv(1). */
		char *tmp_cmd[] = {"mv", "--", file, dest, NULL};
		ret = launch_execv(tmp_cmd, FOREGROUND, E_NOFLAG);
		mvcmd = 1;
	}

	free(dest);

	if (ret != EXIT_SUCCESS) {
		if (mvcmd == 1)
			xerror(_("trash: %s: Error moving file to Trash\n"), file);
		else
			xerror(_("trash: %s: %s\n"), file, strerror(errno));
		free(file_suffix);
		return errno;
	}

	/* Generate the info file */
	len = strlen(trash_info_dir) + strlen(file_suffix) + 12;
	char *info_file = (char *)xnmalloc(len, sizeof(char));
	snprintf(info_file, len, "%s/%s.trashinfo", trash_info_dir, file_suffix);

	int fd = 0;
	FILE *info_fp = open_fwrite(info_file, &fd);
	if (!info_fp) {
		xerror("trash: %s: %s\n", info_file, strerror(errno));
		return del_trash_file_and_exit(&file_suffix, &info_file);
	}

	ret = EXIT_SUCCESS;

	/* Encode path to URL format (RF 2396) */
	char *url_str = url_encode(tmpfile);
	if (!url_str) {
		xerror(_("trash: %s: Error encoding path\n"), file);
		ret = EXIT_FAILURE;
		goto END;
	}

	fprintf(info_fp,
	    "[Trash Info]\nPath=%s\nDeletionDate=%d-%d-%dT%d:%d:%d\n",
	    url_str, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
	    tm->tm_hour, tm->tm_min, tm->tm_sec);

	free(url_str);

END:
	fclose(info_fp);
	free(info_file);
	free(file_suffix);
	return ret;
}

/* Remove NAME file and the corresponding .trashinfo file from the trash can */
static int
remove_file_from_trash(const char *name)
{
	char rm_file[PATH_MAX], rm_info[PATH_MAX];
	snprintf(rm_file, sizeof(rm_file), "%s/%s", trash_files_dir, name);
	snprintf(rm_info, sizeof(rm_info), "%s/%s.trashinfo", trash_info_dir, name);

	int tmp_err = 0, err_file = 0, err_info = 0;
	struct stat a;
	if (stat(rm_file, &a) == -1) {
		xerror("trash: %s: %s\n", rm_file, strerror(errno));
		err_file = tmp_err = errno;
	}
	if (stat(rm_info, &a) == -1) {
		if (err_file == EXIT_SUCCESS)
			xerror("trash: %s: %s\n", rm_info, strerror(errno));
		err_info = tmp_err = errno;
	}

	if (err_file != EXIT_SUCCESS || err_info != EXIT_SUCCESS)
		return tmp_err;

	char *cmd[] = {"rm", "-rf", "--", rm_file, rm_info, NULL};
	return launch_execv(cmd, FOREGROUND, E_NOFLAG);
}

static int
remove_from_trash_params(char **args)
{
	size_t rem_files = 0;
	size_t i;
	int exit_status = EXIT_SUCCESS;

	for (i = 0; args[i]; i++) {
		if (*args[i] == '*' && !args[i][1])
			return trash_clear();

		char *d = (char *)NULL;
		if (strchr(args[i], '\\'))
			d = dequote_str(args[i], 0);

		if (remove_file_from_trash(d ? d : args[i]) != EXIT_SUCCESS)
			exit_status = EXIT_FAILURE;
		else
			rem_files++;

		free(d);
	}

	if (conf.autols == 1 && exit_status == EXIT_SUCCESS)
		reload_dirlist();

	print_reload_msg(_("%zu file(s) removed from the trash can\n"), rem_files);
	print_reload_msg(_("%zu total trashed file(s)\n"), trash_n - rem_files);

	return exit_status;
}

static int
remove_from_trash(char **args)
{
	/* Remove from trash files passed as parameters */
	if (args[2])
		return remove_from_trash_params(args + 2);

	int exit_status = EXIT_SUCCESS;
	size_t i;

	/* No parameters: list, take input, and remove */

	/* 1) List trashed files */
	/* Change CWD to the trash directory. Otherwise, scandir(3) will fail */
	if (xchdir(trash_files_dir, NO_TITLE) == -1) {
		xerror("trash: %s: %s\n", trash_files_dir, strerror(errno));
		return EXIT_FAILURE;
	}

	struct dirent **trash_files = (struct dirent **)NULL;
	int files_n = scandir(trash_files_dir, &trash_files,
			skip_files, conf.unicode == 1 ? alphasort
			: (conf.case_sens_list == 1 ? xalphasort : alphasort_insensitive));

	if (files_n <= -1) {
		xerror("trash: %s: %s\n", trash_files_dir, strerror(errno));
		return EXIT_FAILURE;
	} else if (files_n > 0) {
		printf(_("%sTrashed files%s\n\n"), BOLD, df_c);
		for (i = 0; i < (size_t)files_n; i++) {
			colors_list(trash_files[i]->d_name, (int)i + 1, NO_PAD,
			    PRINT_NEWLINE);
		}
	} else {
		puts(_("trash: No trashed files"));
		/* Restore CWD and return */
		if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1)
			xerror("trash: %s: %s\n", workspaces[cur_ws].path, strerror(errno));

		return EXIT_SUCCESS;
	}

	/* Restore CWD and continue */
	if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
		xerror("trash: %s: %s\n", workspaces[cur_ws].path, strerror(errno));
		return EXIT_FAILURE;
	}

	/* 2) Get user input */
	printf(_("\n%sEnter 'q' to quit\n"
		"File(s) to be removed (ex: 1 2-6, or *):\n"), df_c);

	char *line = (char *)NULL, **rm_elements = (char **)NULL;
	char tp[(MAX_COLOR * 2) + 7];
	snprintf(tp, sizeof(tp), "\001%s\002>\001%s\002 ", mi_c, tx_c);

	while (!line)
		line = rl_no_hist(tp);

	rm_elements = get_substr(line, ' ');
	free(line);

	if (!rm_elements)
		return EXIT_FAILURE;

	/* 3) Remove files */
	int ret = -1;

	/* First check for exit, wildcard, and non-number args */
	for (i = 0; rm_elements[i]; i++) {
		/* Quit */
		if (strcmp(rm_elements[i], "q") == 0) {
			size_t j;
			for (j = 0; rm_elements[j]; j++)
				free(rm_elements[j]);
			free(rm_elements);

			for (j = 0; j < (size_t)files_n; j++)
				free(trash_files[j]);
			free(trash_files);

			if (conf.autols == 1)
				reload_dirlist();

			return exit_status;
		}

		/* Asterisk */
		else if (strcmp(rm_elements[i], "*") == 0) {
			size_t j, removed_files = 0;
			for (j = 0; j < (size_t)files_n; j++) {
				ret = remove_file_from_trash(trash_files[j]->d_name);
				if (ret != EXIT_SUCCESS) {
					xerror(_("trash: %s: Cannot remove file from the "
						"trash can\n"), trash_files[j]->d_name);
					exit_status = EXIT_FAILURE;
				} else {
					removed_files++;
				}

				free(trash_files[j]);
			}

			free(trash_files);

			for (j = 0; rm_elements[j]; j++)
				free(rm_elements[j]);
			free(rm_elements);

			if (conf.autols == 1)
				reload_dirlist();

			print_reload_msg(_("%zu file(s) removed from the trash can\n"),
				removed_files);

			return exit_status;
		}

		else if (!is_number(rm_elements[i])) {
			xerror(_("trash: %s: Invalid ELN\n"), rm_elements[i]);
			exit_status = EXIT_FAILURE;

			size_t j;
			for (j = 0; rm_elements[j]; j++)
				free(rm_elements[j]);
			free(rm_elements);

			for (j = 0; j < (size_t)files_n; j++)
				free(trash_files[j]);
			free(trash_files);

			return exit_status;
		}
	}

	size_t removed_files = 0;
	/* If all args are numbers, and neither 'q' nor wildcard */
	for (i = 0; rm_elements[i]; i++) {
		int rm_num = atoi(rm_elements[i]);
		if (rm_num <= 0 || rm_num > files_n) {
			xerror(_("trash: %d: Invalid ELN\n"), rm_num);
			free(rm_elements[i]);
			exit_status = EXIT_FAILURE;
			continue;
		}

		ret = remove_file_from_trash(trash_files[rm_num - 1]->d_name);
		if (ret != EXIT_SUCCESS) {
			xerror(_("trash: %s: Cannot remove file from the trash can\n"),
				trash_files[rm_num - 1]->d_name);
			exit_status = EXIT_FAILURE;
		} else {
			removed_files++;
		}

		free(rm_elements[i]);
	}

	free(rm_elements);

	for (i = 0; i < (size_t)files_n; i++)
		free(trash_files[i]);
	free(trash_files);

	if (conf.autols == 1)
		reload_dirlist();

	print_reload_msg(_("%zu file(s) removed from the trash can\n"),
		removed_files);

	return exit_status;
}

static int
untrash_file(char *file)
{
	if (!file)
		return EXIT_FAILURE;

	char undel_file[PATH_MAX], undel_info[PATH_MAX];
	snprintf(undel_file, sizeof(undel_file), "%s/%s", trash_files_dir, file);
	snprintf(undel_info, sizeof(undel_info), "%s/%s.trashinfo",
		trash_info_dir, file);

	FILE *info_fp;
	info_fp = fopen(undel_info, "r");
	if (!info_fp) {
		xerror(_("undel: Info file for '%s' not found. Try restoring "
			"the file manually\n"), file);
		return errno;
	}

	char *orig_path = (char *)NULL;
	/* The max length for line is Path=(5) + PATH_MAX + \n(1) */
	char line[PATH_MAX + 6];
	memset(line, '\0', sizeof(line));

	while (fgets(line, (int)sizeof(line), info_fp)) {
		if (strncmp(line, "Path=", 5) == 0) {
			char *p = strchr(line, '=');
			if (!p || !*(++p))
				break;
			orig_path = savestring(p, strlen(p));
		}
	}

	fclose(info_fp);

	/* If original path is NULL or empty, return error */
	if (!orig_path)
		return EXIT_FAILURE;

	if (*orig_path == '\0') {
		free(orig_path);
		return EXIT_FAILURE;
	}

	/* Remove new line char from original path, if any */
	size_t orig_path_len = strlen(orig_path);
	if (orig_path_len > 0 && orig_path[orig_path_len - 1] == '\n')
		orig_path[orig_path_len - 1] = '\0';

	/* Decode original path's URL format */
	char *url_decoded = url_decode(orig_path);
	if (!url_decoded) {
		xerror(_("undel: %s: Error decoding original path\n"), orig_path);
		free(orig_path);
		return EXIT_FAILURE;
	}

	free(orig_path);
	orig_path = (char *)NULL;

	/* Check existence and permissions of parent directory */
	char *parent = (char *)NULL;
	parent = strbfrlst(url_decoded, '/');
	if (!parent) {
		/* strbfrlst() returns NULL is file's parent is root (simply
		 * because there's nothing before last slash in this case).
		 * So, check if file's parent is root. Else return */
		if (url_decoded[0] == '/' && strcntchr(url_decoded + 1, '/') == -1) {
			parent = (char *)xnmalloc(2, sizeof(char));
			parent[0] = '/';
			parent[1] = '\0';
		} else {
			free(url_decoded);
			return EXIT_FAILURE;
		}
	}

	if (access(parent, F_OK | X_OK | W_OK) != 0) {
		xerror("undel: %s: %s\n", parent, strerror(errno));
		free(parent);
		free(url_decoded);
		return errno;
	}

	free(parent);

	struct stat a;
	if (stat(url_decoded, &a) != -1) {
		xerror(_("undel: %s: Destination file exists\n"), url_decoded);
		free(url_decoded);
		return EEXIST;
	}

	int ret = renameat(XAT_FDCWD, undel_file, XAT_FDCWD, url_decoded);
	if (ret == -1) {
		if (errno == EXDEV) {
			/* Destination file is on a different file system, which is why
			 * rename(3) doesn't work: let's try with mv(1). */
			char *cmd[] = {"mv", "--", undel_file, url_decoded, NULL};
			ret = launch_execv(cmd, FOREGROUND, E_NOFLAG);
			if (ret != EXIT_SUCCESS) {
				free(url_decoded);
				return ret;
			}
		} else {
			xerror("undel: %s: %s\n", undel_file, strerror(errno));
			free(url_decoded);
			return errno;
		}
	}

	free(url_decoded);

	char *cmd[] = {"rm", "-rf", "--", undel_info, NULL};
	ret = launch_execv(cmd, FOREGROUND, E_NOFLAG);
	if (ret != EXIT_SUCCESS) {
		xerror(_("undel: %s: Error removing info file\n"), undel_info);
		return ret;
	}

	return EXIT_SUCCESS;
}

/* Untrash all trashed files. */
static int
untrash_all(struct dirent ***trash_files, const int trash_files_n)
{
	size_t j;
	int exit_status = EXIT_SUCCESS;

	for (j = 0; j < (size_t)trash_files_n; j++) {
		if (untrash_file((*trash_files)[j]->d_name) != 0)
			exit_status = EXIT_FAILURE;
		free((*trash_files)[j]);
	}
	free(*trash_files);

	if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
		err(0, NOPRINT_PROMPT, "undel: %s: %s\n",
		    workspaces[cur_ws].path, strerror(errno));
		return EXIT_FAILURE;
	}

	if (conf.autols == 1)
		reload_dirlist();
	print_reload_msg(_("0 trashed files\n"));

	return exit_status;
}

/* Untrash files passed as parameters (ARGS). */
static int
untrash_files(char **args)
{
	int exit_status = EXIT_SUCCESS;
	size_t i, untrashed_files = 0;

	for (i = 0; args[i]; i++) {
		char *d = (char *)NULL;
		if (strchr(args[i], '\\'))
			d = dequote_str(args[i], 0);

		if (untrash_file(d ? d : args[i]) != EXIT_SUCCESS)
			exit_status = EXIT_FAILURE;
		else
			untrashed_files++;

		free(d);
	}

	if (exit_status == EXIT_SUCCESS) {
		size_t n = count_trashed_files();

		if (conf.autols == 1)
			reload_dirlist();

		print_reload_msg(_("%zu file(s) untrashed\n"), untrashed_files);
		print_reload_msg(_("%zu total trashed file(s)\n"), n);
	}

	return exit_status;
}

int
untrash_function(char **comm)
{
	if (!comm)
		return EXIT_FAILURE;

	if (trash_ok == 0 || !trash_dir || !trash_files_dir || !trash_info_dir) {
		xerror(_("%s: Trash function disabled\n"), PROGRAM_NAME);
		return EXIT_FAILURE;
	}

	int exit_status = EXIT_SUCCESS;

	if (comm[1] && *comm[1] != '*' && strcmp(comm[1], "a") != 0
	&& strcmp(comm[1], "all") != 0)
		return untrash_files(comm + 1);

	/* Change CWD to the trash directory to make scandir(3) work. */
	if (xchdir(trash_files_dir, NO_TITLE) == -1) {
		err(0, NOPRINT_PROMPT, "undel: %s: %s\n",
			trash_files_dir, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Get trashed files */
	struct dirent **trash_files = (struct dirent **)NULL;
	int trash_files_n = scandir(trash_files_dir, &trash_files,
	    skip_files, (conf.unicode) ? alphasort : (conf.case_sens_list)
			? xalphasort : alphasort_insensitive);
	if (trash_files_n <= 0) {
		puts(_("trash: No trashed files"));

		if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
			err(0, NOPRINT_PROMPT, "undel: %s: %s\n",
			    workspaces[cur_ws].path, strerror(errno));
			return EXIT_FAILURE;
		}

		return EXIT_SUCCESS;
	}

	/* if "undel all" (or "u a" or "u *") */
	if (comm[1] && (strcmp(comm[1], "*") == 0 || strcmp(comm[1], "a") == 0
	|| strcmp(comm[1], "all") == 0))
		return untrash_all(&trash_files, trash_files_n);

	/* List trashed files */
	printf(_("%sTrashed files%s\n\n"), BOLD, df_c);
	size_t i;
	uint8_t tpad = DIGINUM(trash_files_n);

	for (i = 0; i < (size_t)trash_files_n; i++) {
		printf("%s%*zu%s ", el_c, tpad, i + 1, df_c);
		colors_list(trash_files[i]->d_name, NO_ELN, NO_PAD, PRINT_NEWLINE);
	}

	/* Go back to previous path */
	if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
		err(0, NOPRINT_PROMPT, "undel: %s: %s\n",
			workspaces[cur_ws].path, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Get user input */
	printf(_("\n%sEnter 'q' to quit\n"
		"File(s) to be undeleted (ex: 1 2-6, or *):\n"), df_c);

	char up[(MAX_COLOR * 2) + 7];
	snprintf(up, sizeof(up), "\001%s\002>\001%s\002 ", mi_c, tx_c);

	int undel_n = 0;
	char *line = (char *)NULL, **undel_elements = (char **)NULL;
	while (!line)
		line = rl_no_hist(up);

	undel_elements = get_substr(line, ' ');
	free(line);
	line = (char *)NULL;
	if (undel_elements) {
		for (i = 0; undel_elements[i]; i++)
			undel_n++;
	} else {
		return EXIT_FAILURE;
	}

	/* First check for quit, *, and non-number args */
	int free_and_return = 0, reload_files = 0;

	for (i = 0; i < (size_t)undel_n; i++) {
		if (strcmp(undel_elements[i], "q") == 0) {
			free_and_return = reload_files = 1;
		} else if (strcmp(undel_elements[i], "*") == 0) {
			size_t j;
			for (j = 0; j < (size_t)trash_files_n; j++)
				if (untrash_file(trash_files[j]->d_name) != 0)
					exit_status = EXIT_FAILURE;

			free_and_return = 1;
		} else if (!is_number(undel_elements[i])) {
			xerror(_("undel: %s: Invalid ELN\n"), undel_elements[i]);
			exit_status = EXIT_FAILURE;
			free_and_return = 1;
		}
	}

	/* Free and return if any of the above conditions is true. */
	if (free_and_return == 1) {
		size_t j = 0;
		for (j = 0; j < (size_t)undel_n; j++)
			free(undel_elements[j]);
		free(undel_elements);

		for (j = 0; j < (size_t)trash_files_n; j++)
			free(trash_files[j]);
		free(trash_files);

		if (conf.autols == 1 && reload_files == 1)
			reload_dirlist();

		return exit_status;
	}

	/* Undelete trashed files */
	for (i = 0; i < (size_t)undel_n; i++) {
		int undel_num = atoi(undel_elements[i]);

		if (undel_num <= 0 || undel_num > trash_files_n) {
			xerror(_("undel: %d: Invalid ELN\n"), undel_num);
			free(undel_elements[i]);
			continue;
		}

		/* If valid ELN */
		if (untrash_file(trash_files[undel_num - 1]->d_name) != EXIT_SUCCESS)
			exit_status = EXIT_FAILURE;

		free(undel_elements[i]);
	}

	free(undel_elements);

	/* Free trashed files list */
	for (i = 0; i < (size_t)trash_files_n; i++)
		free(trash_files[i]);
	free(trash_files);

	/* If some trashed file still remains, reload the undel screen */
	trash_n = (size_t)count_dir(trash_files_dir, NO_CPOP);

	if (trash_n <= 2)
		trash_n = 0;

	if (trash_n)
		untrash_function(comm);

	return exit_status;
}

/* List files currently in the trash can */
static int
list_trashed_files(void)
{
	if (xchdir(trash_files_dir, NO_TITLE) == -1) {
		err(0, NOPRINT_PROMPT, "trash: %s: %s\n",
			trash_files_dir, strerror(errno));
		return EXIT_FAILURE;
	}

	struct dirent **trash_files = (struct dirent **)NULL;
	int files_n = scandir(trash_files_dir, &trash_files,
			skip_files, (conf.unicode) ? alphasort : (conf.case_sens_list)
			? xalphasort : alphasort_insensitive);

	if (files_n == -1) {
		xerror("trash: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	if (files_n <= 0) {
		puts(_("trash: No trashed files"));
		return (-1);
	}

	uint8_t tpad = DIGINUM(files_n);
	size_t i;
	for (i = 0; i < (size_t)files_n; i++) {
		printf("%s%*zu%s ", el_c, tpad, i + 1, df_c);
		colors_list(trash_files[i]->d_name, NO_ELN, NO_PAD, PRINT_NEWLINE);
		free(trash_files[i]);
	}
	free(trash_files);

	if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
		err(0, NOPRINT_PROMPT, "trash: %s: %s\n",
			workspaces[cur_ws].path, strerror(errno));
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

/* Make sure we are trashing a valid file */
static int
check_trash_file(char *deq_file)
{
	char tmp_cmd[PATH_MAX];
	if (*deq_file == '/') /* If absolute path */
		xstrsncpy(tmp_cmd, deq_file, sizeof(tmp_cmd));
	else /* If relative path, add path to check against TRASH_DIR */
		snprintf(tmp_cmd, sizeof(tmp_cmd), "%s/%s",
			workspaces[cur_ws].path, deq_file);

	/* Do not trash any of the parent directories of TRASH_DIR */
	if (strncmp(tmp_cmd, trash_dir, strlen(tmp_cmd)) == 0) {
		xerror(_("trash: Cannot trash '%s'\n"), tmp_cmd);
		return EXIT_FAILURE;
	}

	/* Do no trash TRASH_DIR itself nor anything inside it (trashed files) */
	if (strncmp(tmp_cmd, trash_dir, strlen(trash_dir)) == 0) {
		puts(_("trash: Use 'trash del' to remove trashed files"));
		return EXIT_FAILURE;
	}

	size_t l = strlen(deq_file);
	if (l > 0 && deq_file[l - 1] == '/')
		/* Do not trash (move) symlinks ending with a slash. According to 'info mv':
		 * "_Warning_: Avoid specifying a source name with a trailing slash, when
		 * it might be a symlink to a directory. Otherwise, 'mv' may do something
		 * very surprising, since its behavior depends on the underlying rename
		 * system call. On a system with a modern Linux-based kernel, it fails
		 * with 'errno=ENOTDIR'.  However, on other systems (at least FreeBSD 6.1
		 * and Solaris 10) it silently renames not the symlink but rather the
		 * directory referenced by the symlink." */
		deq_file[l - 1] = '\0';

	struct stat a;
	if (lstat(deq_file, &a) == -1) {
		xerror(_("trash: %s: %s\n"), deq_file, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Do not trash block or character devices */
	if (S_ISBLK(a.st_mode) || S_ISCHR(a.st_mode)) {
		xerror(_("trash: %s: Cannot trash a %s device\n"), deq_file,
			S_ISCHR(a.st_mode) ? _("character") : _("block"));
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

/* Print the list of successfully trashed files. */
static void
print_trashed_files(char **args, const int *trashed, const size_t trashed_n)
{
	if (print_removed_files == 0)
		return;

	size_t i;
	for (i = 0; i < trashed_n; i++) {
		if (!args[trashed[i]] || !*args[trashed[i]])
			continue;

		char *p = args[trashed[i]];
		if (strchr(args[trashed[i]], '\\')
		&& !(p = dequote_str(args[trashed[i]], 0)) ) {
			xerror("trash: %s: Error dequoting file name\n", args[trashed[i]]);
			continue;
		}

		char *tmp = abbreviate_file_name(p);
		if (!tmp) {
			xerror("trash: %s: Error abbreviating file name\n", p);
			if (p && p != args[trashed[i]])
				free(p);
			continue;
		}

		char *name = (*tmp == '.' && tmp[1] == '/') ? tmp + 2 : tmp;

		puts(name);

		if (tmp && tmp != p)
			free(tmp);
		if (p && p != args[trashed[i]])
			free(p);
	}
}

/* Trash files passed as arguments to the trash command */
static int
trash_files_args(char **args)
{
	time_t rawtime = time(NULL);
	struct tm t;
	char *suffix = localtime_r(&rawtime, &t) ? gen_date_suffix(t) : (char *)NULL;
	if (!suffix)
		return EXIT_FAILURE;

	int exit_status = EXIT_SUCCESS, cwd = 0;
	size_t i, trashed_files = 0, n = 0;
	for (i = 1; args[i]; i++);
	int *successfully_trashed = (int *)xnmalloc(i + 1, sizeof(int));

	for (i = 1; args[i]; i++) {
		char *deq_file = dequote_str(args[i], 0);
		if (!deq_file) {
			xerror("trash: %s: Error dequoting file\n", args[i]);
			continue;
		}

		/* Make sure we are trashing a valid file */
		if (check_trash_file(deq_file) == EXIT_FAILURE) {
			exit_status = EXIT_FAILURE;
			free(deq_file);
			continue;
		}

		if (cwd == 0)
			cwd = is_file_in_cwd(deq_file);

		/* Once here, everything is fine: trash the file */
		if (trash_file(suffix, &t, deq_file) == EXIT_SUCCESS) {
			trashed_files++;
			if (print_removed_files == 1) {
				/* Store indices of successfully trashed files */
				successfully_trashed[n] = (int)i;
				n++;
			}
		} else {
			cwd = 0;
			exit_status = EXIT_FAILURE;
		}

		free(deq_file);
	}

	free(suffix);

	if (exit_status == EXIT_SUCCESS) {
		if (conf.autols == 1 && cwd == 1)
			reload_dirlist();
		print_trashed_files(args, successfully_trashed, n);
		print_reload_msg(_("%zu file(s) trashed\n"), trashed_files);
		print_reload_msg(_("%zu total trashed file(s)\n"),
			trash_n + trashed_files);
	} else if (trashed_files > 0) {
		/* An error occured, but at least one file was trashed as well.
		 * If this file was in the current dir, the screen will be refreshed
		 * after this function (by inotify/kqueue), hidding the error message.
		 * So let's pause here to prevent the error from being hidden, and
		 * then refresh the list of files ourselves. */
		if (conf.autols == 1) {
			fputs(_("Press any key to continue... \n"), stderr);
			xgetchar();
			reload_dirlist();
		}

		print_trashed_files(args, successfully_trashed, n);
		print_reload_msg(_("%zu file(s) trashed\n"), trashed_files);
		print_reload_msg(_("%zu total trashed file(s)\n"),
			trash_n + trashed_files);
	}

	free(successfully_trashed);
	return exit_status;
}

int
trash_function(char **args)
{
	if (!args)
		return EXIT_FAILURE;

	if (trash_ok == 0 || !trash_dir || !trash_info_dir || !trash_files_dir) {
		xerror(_("%s: Trash function disabled\n"), PROGRAM_NAME);
		return EXIT_FAILURE;
	}

	/* List trashed files ('tr' or 'tr ls') */
	if (!args[1] || (*args[1] == 'l'
	&& (strcmp(args[1], "ls") == 0 || strcmp(args[1], "list") == 0))) {
		int ret = list_trashed_files();
		if (ret == -1 || ret == EXIT_SUCCESS)
			return EXIT_SUCCESS;
		return EXIT_FAILURE;
	}

	trash_n = count_trashed_files();

	if (*args[1] == 'd' && strcmp(args[1], "del") == 0)
		return remove_from_trash(args);

	if ((*args[1] == 'c' && strcmp(args[1], "clear") == 0)
	|| (*args[1] == 'e' && strcmp(args[1], "empty") == 0))
		return trash_clear();
	else {
		return trash_files_args(args);
	}
}
#else
void *_skip_me_trash;
#endif /* !_NO_TRASH */
