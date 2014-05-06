/*
    Copyright (C) 2013  ABRT Team
    Copyright (C) 2009  Zdenek Prikryl (zprikryl@redhat.com)
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "internal_libreport.h"
#include <augeas.h>
#include <libgen.h>

/* Cannot use realpath() only because the function doesn't work for
 * non-existing files.
 *
 * libreport uses these path patterns:
 * /etc/libreport/plugins/cool-plugin.conf
 * $HOME/.config/abrt/settings/sweat-plugin.conf
 *
 * unit test uses relative paths:
 * ../../conf/default/file.conf
 */
static bool canonicalize_path(const char *path, char *canon_path)
{
    if (realpath(path, canon_path) != NULL)
        return true;

    if (errno != ENOENT && errno != ENOTDIR)
        return false;

    bool retval = false;

    char *dirc = xstrdup(path);
    char *basec = xstrdup(path);
    /* Be ware that dirname("..") == ".." */
    char *dname = dirname(dirc);
    /* Be ware that basename("/"|".."|".") == "/"|".."|"." */
    char *bname = basename(basec);

    /* Don't want to support paths like '/root/notexist/notexist2/../file.conf' */
    if (strcmp(bname, "..") == 0
        || strcmp(bname, ".") == 0
        /* Cannot get realpath for "/", how this could happened? */
        || strcmp(bname, "/") == 0
        /* It seems that CWD does not exist (CDW + ../file.conf)*/
        || strcmp(dname, "..") == 0
        /* Here as well as above */
        || strcmp(dname, ".") == 0)
    {
        errno = ENOTDIR;
        goto canon_cleanup;
    }

    if (!canonicalize_path(dname, canon_path))
        goto canon_cleanup;

    size_t canon_path_len = strlen(canon_path);
    const size_t bname_len = strlen(bname);
    /* + 1 for slash */
    if (canon_path_len + bname_len + 1 >= PATH_MAX)
    {
        errno = ENAMETOOLONG;
        goto canon_cleanup;
    }

    if (canon_path[canon_path_len - 1] != '/')
    {
        canon_path[canon_path_len] = '/';
        ++canon_path_len;
        canon_path[canon_path_len] = '\0';
    }

    strcpy(canon_path + canon_path_len, bname);
    retval = true;

canon_cleanup:
    free(dirc);
    free(basec);

    return retval;
}

/* augeas refuses to create a directories :(
 * http://www.redhat.com/archives/augeas-devel/2008-November/msg00012.html
 * https://bugzilla.redhat.com/show_bug.cgi?id=1032562
 */
static bool create_parentdir(char *path)
{
    bool ret;
    char *c;

    c = g_path_get_dirname(path);
    ret = g_mkdir_with_parents(c, 0700);
    g_free(c);

    return ret;
}

static void internal_aug_error_msg(augeas *aug, const char *def_msg)
{
    const char *err_msg = def_msg;

    //if (aug_error(aug) != AUG_NOERROR)
    {
        /* Try to get the most detailed error. */
        err_msg = aug_error_details(aug);
        error_msg("%s", err_msg);

        if (err_msg == NULL)
            /* The detailed error message was not provided. */
            /* Try to get a message elaborating the error code. */
            err_msg = aug_error_minor_message(aug);

        error_msg("%s", err_msg);
        if (err_msg == NULL)
            /* No more detailed message for the error code. */
            /* Get a message for the error code. */
            err_msg = aug_error_message(aug);

        error_msg("%s", err_msg);
    }

    error_msg("%s", err_msg);
}

/* Only for debugging */
#define NOAUTOLOAD 1

/* Initializes augeas for libreport only
 *
 * The default initialization of augeas takes ages because it parses all
 * configuration files which are known to augeas. Unfortunately we do want work
 * only with the libreport configuration files. Hence, we have to disable the
 * autoloading and configure augeas to load only the Libreport module.
 *
 * The list of loaded modules is stored in /augeas/load/\* nodes. If the
 * autoloading is enabled, the path is populated with all augeas modules.  But
 * in our case the path is empty (there is no child node at /augeas/load/ path)
 * so we have to add Libreport node (aug_set() creates a new node if it not
 * exists yet) and configure the lens and the included files for Libreport
 * node.
 */
static bool
internal_aug_init(augeas **aug, const char *path)
{
    int aug_flag = AUG_NO_ERR_CLOSE /* without this flag aug_init() returns
                                     * NULL on errors and we cannot read the
                                     * error message
                                     */
#if NOAUTOLOAD
                 | AUG_NO_MODL_AUTOLOAD /* do not parse all configuration files
                                         * in the system (empty /augeas/load)
                                         */
#endif
                 ;

    *aug = aug_init(/* default root */ NULL, /* default lens lib */ NULL, aug_flag);

    if (aug_error(*aug) != AUG_NOERROR)
    {
        error_msg("%s", aug_error_message(*aug));
        return false;
    }

#if NOAUTOLOAD
    /* Libreport.lns -> use the lens from the Libreport module. */
    /* Cannot use '@Libreport' (Libreport module) because this construction works */
    /* only for autoloaded modules (augeas-1.1.0) */
    if (aug_set(*aug, "/augeas/load/Libreport/lens", "Libreport.lns") < 0)
    {
        internal_aug_error_msg(*aug, "Cannot configure augeas to use Libreport.lns");
        return false;
    }

    /* parse only this configuration file */
    if (aug_set(*aug, "/augeas/load/Libreport/incl[1]", path) < 0)
    {
        internal_aug_error_msg(*aug, "Cannot configure augeas to read the configuration file");
        return false;
    }

    if (aug_load(*aug) < 0)
    {
        internal_aug_error_msg(*aug, "Cannot load the configuration file");
        return false;
    }
#endif

    return true;
}

/* Returns false if any error occurs, else returns true.
 */
bool load_conf_file(const char *path, map_string_t *settings, bool skipKeysWithoutValue)
{
    bool retval = false;
    char real_path[PATH_MAX + 1];
    augeas *aug = NULL;

    if (!canonicalize_path(path, real_path))
    {
        perror_msg("Cannot get real path for '%s'", path);
        goto finalize;
    }

    if (!internal_aug_init(&aug, real_path))
        goto finalize;

    char *aug_expr = xasprintf("/files%s/*[label() != \"#comment\"]", real_path);
    char **matches = NULL;
    const int match_num = aug_match(aug, aug_expr, &matches);
    free(aug_expr);

    if (match_num < 0)
    {
        internal_aug_error_msg(aug, "An error occurred while searching for configuration options");
        goto finalize;
    }
    if (match_num == 0)
    {
        struct stat buf;
        if (0 != stat(real_path, &buf))
        {
            /* We expect that the path doesn't exist, therefore print ENOENT */
            /* message in verbose mode and all other error messages in */
            /* non-verbose mode. */
            if (errno != ENOENT || g_verbose > 1)
                perror_msg("Cannot read conf file '%s'", real_path);
        }
        else if (!S_ISREG(buf.st_mode))
        {
            /* A user should know that the path to configuration file is not */
            /* a regular file. */
            error_msg("Configuration path '%s' is not a regular file", real_path);
        }
        else
        {
            retval = true;
            VERB1 log("Configuration file '%s' contains no option", real_path);
        }
        goto finalize;
    }

    int i = 0;
    for (; i < match_num; ++i)
    {
        const char *option = strrchr(matches[i], '/') + 1;
        const char *value = NULL;
        const int ret = aug_get(aug, matches[i], &value);
        if (ret == 0)
        {
            log("Option '%s' disappeared from '%s' while parsing", option, real_path);
            goto cleanup;
        }
        if (ret == -1)
        {
            internal_aug_error_msg(aug, "An error occurred while retrieving an option's value");
            goto cleanup;
        }

        VERB1 log("Loaded option '%s' = '%s'", option, value);

        if (!skipKeysWithoutValue || value[0] != '\0')
            replace_map_string_item(settings, xstrdup(option), xstrdup(value));

        free(matches[i]);
    }
    retval = true;

cleanup:
    for (; i < match_num; ++i)
        free(matches[i]);
    free(matches);

finalize:
    if (aug != NULL)
        aug_close(aug);

    return retval;
}

bool load_conf_file_from_dirs(const char *base_name, const char *const *directories, map_string_t *settings, bool skipKeysWithoutValue)
{
    if (NULL == directories || NULL == *directories)
    {
        error_msg("No configuration directory specified");
        return false;
    }

    bool result = true;
    for (const char *const *dir = directories; *dir != NULL; ++dir)
    {
        char *conf_file = concat_path_file(*dir, base_name);
        if (!load_conf_file(conf_file, settings, skipKeysWithoutValue))
        {
            perror_msg("Can't open '%s'", conf_file);
            result = false;
        }
        free(conf_file);
    }

    return result;
}

/* Returns false if saving failed */
bool save_conf_file(const char *path, map_string_t *settings)
{
    bool retval = false;
    char real_path[PATH_MAX + 1];
    augeas *aug = NULL;

    if (!canonicalize_path(path, real_path))
    {
        perror_msg("Cannot get real path for '%s'", path);
        goto finalize;
    }

    if (!internal_aug_init(&aug, real_path))
        goto finalize;

    const char *name = NULL;
    const char *value = NULL;
    map_string_iter_t iter;
    init_map_string_iter(&iter, settings);
    while (next_map_string_iter(&iter, &name, &value))
    {
        char *aug_path = xasprintf("/files%s/%s", real_path, name);
        const int ret = aug_set(aug, aug_path, value);
        free(aug_path);
        if (ret < 0)
        {
            internal_aug_error_msg(aug, "Cannot set a value of a tree path");
            goto finalize;
        }
    }

    create_parentdir(real_path);
    if (aug_save(aug) < 0)
    {
        internal_aug_error_msg(aug, "Cannot save the changes made to the configuration");
        goto finalize;
    }

    retval = true;

finalize:
    if (aug != NULL)
        aug_close(aug);

    return retval;
}
