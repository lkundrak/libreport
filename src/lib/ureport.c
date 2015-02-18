/*
    Copyright (C) 2012,2014  ABRT team
    Copyright (C) 2012,2014  RedHat Inc

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

#include <json.h>

#include <satyr/abrt.h>
#include <satyr/report.h>

#include "internal_libreport.h"
#include "ureport.h"
#include "abrt_curl.h"

#define DESTROYED_POINTER (void *)0xdeadbeef

#define BTHASH_URL_SFX "reports/bthash/"

#define RHSM_WEB_SERVICE_URL "https://api.access.redhat.com/rs/telemetry/abrt"

#define RHSM_CERT_PATH "/etc/pki/consumer/cert.pem"
#define RHSM_KEY_PATH "/etc/pki/consumer/key.pem"

#define RHSMENT_PEM_DIR_PATH "/etc/pki/entitlement"
#define RHSMENT_ENT_DATA_BEGIN_TAG "-----BEGIN ENTITLEMENT DATA-----"
#define RHSMENT_ENT_DATA_END_TAG "-----END ENTITLEMENT DATA-----"
#define RHSMENT_SIG_DATA_BEGIN_TAG "-----BEGIN RSA SIGNATURE-----"
#define RHSMENT_SIG_DATA_END_TAG "-----END RSA SIGNATURE-----"


static char *
puppet_config_print(const char *key)
{
    char *command = xasprintf("puppet config print %s", key);
    char *result = run_in_shell_and_save_output(0, command, NULL, NULL);
    free(command);

    /* run_in_shell_and_save_output always returns non-NULL */
    if (result[0] != '/')
        goto error;

    char *newline = strchrnul(result, '\n');
    if (!newline)
        goto error;

    *newline = '\0';
    return result;
error:
    free(result);
    error_msg_and_die("Unable to determine puppet %s path (puppet not installed?)", key);
}

void
ureport_server_config_set_url(struct ureport_server_config *config,
                              char *server_url)
{
    free(config->ur_url);
    config->ur_url = server_url;
}

void
ureport_server_config_set_client_auth(struct ureport_server_config *config,
                                      const char *client_auth)
{
    if (client_auth == NULL)
        return;

    if (strcmp(client_auth, "") == 0)
    {
        free(config->ur_client_cert);
        config->ur_client_cert = NULL;

        free(config->ur_client_key);
        config->ur_client_key = NULL;
        VERB2 log("Not using client authentication");
    }
    else if (strcmp(client_auth, "rhsm") == 0)
    {
        if (config->ur_url == NULL)
            ureport_server_config_set_url(config, xstrdup(RHSM_WEB_SERVICE_URL));

        config->ur_client_cert = xstrdup(RHSM_CERT_PATH);
        config->ur_client_key = xstrdup(RHSM_KEY_PATH);
    }
    else if (strcmp(client_auth, "rhsm-entitlement") == 0)
    {
        if (config->ur_url == NULL)
            ureport_server_config_set_url(config, xstrdup(RHSM_WEB_SERVICE_URL));

        GList *certs = get_file_list(RHSMENT_PEM_DIR_PATH, "pem");
        if (g_list_length(certs) != 2)
        {
            VERB2 log(RHSMENT_PEM_DIR_PATH" does not contain unique cert-key files pair");
            VERB2 log("Not using client authentication");
            return;
        }

        const char *cert = NULL;
        const char *key = NULL;

        file_obj_t *fst = (file_obj_t *)certs->data;
        file_obj_t *scn = (file_obj_t *)certs->next->data;

        if (strlen(fo_get_filename(fst)) < strlen(fo_get_filename(scn)))
        {
            cert = fo_get_filename(fst);
            key = fo_get_filename(scn);

            config->ur_client_cert = xstrdup(fo_get_fullpath(fst));
            config->ur_client_key = xstrdup(fo_get_fullpath(scn));
        }
        else
        {
            cert = fo_get_filename(scn);
            key = fo_get_filename(fst);

            config->ur_client_cert = xstrdup(fo_get_fullpath(scn));
            config->ur_client_key = xstrdup(fo_get_fullpath(fst));
        }

        const bool iscomplement = prefixcmp(key, cert) != 0 || strcmp("-key", key + strlen(cert)) != 0;
        g_list_free_full(certs, (GDestroyNotify)free_file_obj);

        if (iscomplement)
        {
            VERB2 log("Key file '%s' isn't complement to cert file '%s'",
                    config->ur_client_key, config->ur_client_cert);
            VERB2 log("Not using client authentication");

            free(config->ur_client_cert);
            free(config->ur_client_key);
            config->ur_client_cert = NULL;
            config->ur_client_key = NULL;

            return;
        }

        char *certdata = xmalloc_open_read_close(config->ur_client_cert, /*no size limit*/NULL);
        if (certdata != NULL)
        {
            char *ent_data = xstrdup_between(certdata,
                    RHSMENT_ENT_DATA_BEGIN_TAG, RHSMENT_ENT_DATA_END_TAG);

            char *sig_data = xstrdup_between(certdata,
                    RHSMENT_SIG_DATA_BEGIN_TAG, RHSMENT_SIG_DATA_END_TAG);

            if (ent_data != NULL && sig_data != NULL)
            {
                ent_data = strremovech(ent_data, '\n');
                insert_map_string(config->ur_http_headers,
                        xstrdup("X-RH-Entitlement-Data"),
                        xasprintf(RHSMENT_ENT_DATA_BEGIN_TAG"%s"RHSMENT_ENT_DATA_END_TAG, ent_data));

                sig_data = strremovech(sig_data, '\n');
                insert_map_string(config->ur_http_headers,
                        xstrdup("X-RH-Entitlement-Sig"),
                        xasprintf(RHSMENT_SIG_DATA_BEGIN_TAG"%s"RHSMENT_SIG_DATA_END_TAG, sig_data));
            }
            else
            {
                VERB2 log("Cert file '%s' doesn't contain Entitlement and RSA Signature sections", config->ur_client_cert);
                VERB2 log("Not using HTTP authentication headers");
            }

            free(sig_data);
            free(ent_data);
            free(certdata);
        }
    }
    else if (strcmp(client_auth, "puppet") == 0)
    {
        config->ur_client_cert = puppet_config_print("hostcert");
        config->ur_client_key = puppet_config_print("hostprivkey");
    }
    else
    {
        char *scratch = xstrdup(client_auth);
        config->ur_client_cert = xstrdup(strtok(scratch, ":"));
        config->ur_client_key = xstrdup(strtok(NULL, ":"));
        free(scratch);

        if (config->ur_client_cert == NULL || config->ur_client_key == NULL)
            error_msg_and_die("Invalid client authentication specification");
    }

    if (config->ur_client_cert && config->ur_client_key)
    {
        VERB2 log("Using client certificate: %s", config->ur_client_cert);
        VERB2 log("Using client private key: %s", config->ur_client_key);

        free(config->ur_username);
        config->ur_username = NULL;

        free(config->ur_password);
        config->ur_password = NULL;
    }
}

void
ureport_server_config_set_basic_auth(struct ureport_server_config *config,
                                     const char *login, const char *password)
{
    ureport_server_config_set_client_auth(config, "");

    free(config->ur_username);
    config->ur_username = xstrdup(login);

    free(config->ur_password);
    config->ur_password = xstrdup(password);
}

void
ureport_server_config_load(struct ureport_server_config *config,
                           map_string_t *settings)
{
    UREPORT_OPTION_VALUE_FROM_CONF(settings, "URL", config->ur_url, xstrdup);
    UREPORT_OPTION_VALUE_FROM_CONF(settings, "SSLVerify", config->ur_ssl_verify, string_to_bool);

    const char *client_auth = NULL;
    UREPORT_OPTION_VALUE_FROM_CONF(settings, "SSLClientAuth", client_auth, (const char *));
    ureport_server_config_set_client_auth(config, client_auth);

    /* If SSLClientAuth is configured, include the auth items by default. */
    bool include_auth = !!config->ur_client_cert;
    UREPORT_OPTION_VALUE_FROM_CONF(settings, "IncludeAuthData", include_auth, string_to_bool);

    if (include_auth)
    {
        const char *auth_items = NULL;
        UREPORT_OPTION_VALUE_FROM_CONF(settings, "AuthDataItems", auth_items, (const char *));
        config->ur_prefs.urp_auth_items = parse_list(auth_items);

        if (config->ur_prefs.urp_auth_items == NULL)
            log("IncludeAuthData set to 'yes' but AuthDataItems is empty.");
    }
}

void
ureport_server_config_init(struct ureport_server_config *config)
{
    config->ur_url = NULL;
    config->ur_ssl_verify = true;
    config->ur_client_cert = NULL;
    config->ur_client_key = NULL;
    config->ur_username = NULL;
    config->ur_password = NULL;
    config->ur_http_headers = new_map_string();
    config->ur_prefs.urp_auth_items = NULL;
}

void
ureport_server_config_destroy(struct ureport_server_config *config)
{
    free(config->ur_url);
    config->ur_url = DESTROYED_POINTER;

    free(config->ur_client_cert);
    config->ur_client_cert = DESTROYED_POINTER;

    free(config->ur_client_key);
    config->ur_client_key = DESTROYED_POINTER;

    free(config->ur_username);
    config->ur_username = DESTROYED_POINTER;

    free(config->ur_password);
    config->ur_password = DESTROYED_POINTER;

    g_list_free_full(config->ur_prefs.urp_auth_items, free);
    config->ur_prefs.urp_auth_items = DESTROYED_POINTER;

    free_map_string(config->ur_http_headers);
    config->ur_http_headers = DESTROYED_POINTER;
}

void
ureport_server_response_free(struct ureport_server_response *resp)
{
    if (!resp)
        return;

    free(resp->urr_solution);
    resp->urr_solution = DESTROYED_POINTER;

    g_list_free_full(resp->urr_reported_to_list, g_free);
    resp->urr_reported_to_list = DESTROYED_POINTER;

    free(resp->urr_bthash);
    resp->urr_bthash = DESTROYED_POINTER;

    free(resp->urr_message);
    resp->urr_message = DESTROYED_POINTER;

    free(resp->urr_value);
    resp->urr_value = DESTROYED_POINTER;

    free(resp);
}

static char *
parse_solution_from_json_list(struct json_object *list,
                              GList **reported_to)
{
    json_object *list_elem, *struct_elem;
    const char *cause, *note, *url;
    struct strbuf *solution_buf = strbuf_new();

    const unsigned length = json_object_array_length(list);

    const char *one_format = _("Your problem seems to be caused by %s\n\n%s\n");
    if (length > 1)
    {
        strbuf_append_str(solution_buf, _("Your problem seems to be caused by one of the following:\n"));
        one_format = "\n* %s\n\n%s\n";
    }

    bool empty = true;
    for (unsigned i = 0; i < length; ++i)
    {
        list_elem = json_object_array_get_idx(list, i);
        if (!list_elem)
            continue;

        if (!json_object_object_get_ex(list_elem, "cause", &struct_elem))
            continue;

        cause = json_object_get_string(struct_elem);
        if (!cause)
            continue;

        if (!json_object_object_get_ex(list_elem, "note", &struct_elem))
            continue;

        note = json_object_get_string(struct_elem);
        if (!note)
            continue;

        empty = false;
        strbuf_append_strf(solution_buf, one_format, cause, note);

        if (!json_object_object_get_ex(list_elem, "url", &struct_elem))
            continue;

        url = json_object_get_string(struct_elem);
        if (url)
        {
            char *reported_to_line = xasprintf("%s: URL=%s", cause, url);
            *reported_to = g_list_append(*reported_to, reported_to_line);
        }
    }

    if (empty)
    {
        strbuf_free(solution_buf);
        return NULL;
    }

    return strbuf_free_nobuf(solution_buf);
}

/* reported_to json element should be a list of structures
   {
     "reporter": "Bugzilla",
     "type": "url",
     "value": "https://bugzilla.redhat.com/show_bug.cgi?id=XYZ"
   }
 */
static GList *
parse_reported_to_from_json_list(struct json_object *list)
{
    int i;
    json_object *list_elem, *struct_elem;
    const char *reporter, *value, *type;
    char *reported_to_line, *prefix;
    GList *result = NULL;

    for (i = 0; i < json_object_array_length(list); ++i)
    {
        prefix = NULL;
        list_elem = json_object_array_get_idx(list, i);
        if (!list_elem)
            continue;

        if (!json_object_object_get_ex(list_elem, "reporter", &struct_elem))
            continue;

        reporter = json_object_get_string(struct_elem);
        if (!reporter)
            continue;

        if (!json_object_object_get_ex(list_elem, "value", &struct_elem))
            continue;

        value = json_object_get_string(struct_elem);
        if (!value)
            continue;

        if (!json_object_object_get_ex(list_elem, "type", &struct_elem))
            continue;

        type = json_object_get_string(struct_elem);
        if (type)
        {
            if (strcasecmp("url", type) == 0)
                prefix = xstrdup("URL=");
            else if (strcasecmp("bthash", type) == 0)
                prefix = xstrdup("BTHASH=");
        }

        if (!prefix)
            prefix = xstrdup("");

        reported_to_line = xasprintf("%s: %s%s", reporter, prefix, value);
        free(prefix);

        result = g_list_append(result, reported_to_line);
    }

    return result;
}

/*
 * Reponse samples:
 * {"error":"field 'foo' is required"}
 * {"response":"true"}
 * {"response":"false"}
 */
static struct ureport_server_response *
ureport_server_parse_json(json_object *json)
{
    json_object *obj = NULL;
    if (json_object_object_get_ex(json, "error", &obj))
    {
        struct ureport_server_response *out_response = xzalloc(sizeof(*out_response));
        out_response->urr_is_error = true;
        /*
         * Used to use json_object_to_json_string(obj), but it returns
         * the string in quote marks (") - IOW, json-formatted string.
         */
        out_response->urr_value = xstrdup(json_object_get_string(obj));
        return out_response;
    }

    if (json_object_object_get_ex(json, "result", &obj))
    {
        struct ureport_server_response *out_response = xzalloc(sizeof(*out_response));
        out_response->urr_value = xstrdup(json_object_get_string(obj));

        json_object *message = NULL;
        if (json_object_object_get_ex(json, "message", &message))
            out_response->urr_message = xstrdup(json_object_get_string(message));

        json_object *bthash = NULL;
        if (json_object_object_get_ex(json, "bthash", &bthash))
            out_response->urr_bthash = xstrdup(json_object_get_string(bthash));

        json_object *reported_to_list = NULL;
        if (json_object_object_get_ex(json, "reported_to", &reported_to_list))
            out_response->urr_reported_to_list =
                parse_reported_to_from_json_list(reported_to_list);

        json_object *solutions = NULL;
        if (json_object_object_get_ex(json, "solutions", &solutions))
            out_response->urr_solution =
                parse_solution_from_json_list(solutions, &(out_response->urr_reported_to_list));

        return out_response;
    }

    return NULL;
}

struct ureport_server_response *
ureport_server_response_from_reply(struct abrt_post_state *post_state,
                                   struct ureport_server_config *config)
{
    /* Previously, the condition here was (post_state->errmsg[0] != '\0')
     * however when the server asks for optional client authentication and we do not have the certificates,
     * then post_state->errmsg contains "NSS: client certificate not found (nickname not specified)" even though
     * the request succeeded.
     */
    if (post_state->curl_result != CURLE_OK)
    {
        error_msg(_("Failed to upload uReport to the server '%s' with curl: %s"), config->ur_url, post_state->errmsg);
        return NULL;
    }

    if (post_state->http_resp_code == 404)
    {
        error_msg(_("The URL '%s' does not exist (got error 404 from server)"), config->ur_url);
        return NULL;
    }

    if (post_state->http_resp_code == 500)
    {
        error_msg(_("The server at '%s' encountered an internal error (got error 500)"), config->ur_url);
        return NULL;
    }

    if (post_state->http_resp_code == 503)
    {
        error_msg(_("The server at '%s' currently can't handle the request (got error 503)"), config->ur_url);
        return NULL;
    }

    if (post_state->http_resp_code != 202
            && post_state->http_resp_code != 400
            && post_state->http_resp_code != 413)
    {
        /* can't print better error message */
        error_msg(_("Unexpected HTTP response from '%s': %d"), config->ur_url, post_state->http_resp_code);
        VERB2 log("%s", post_state->body);
        return NULL;
    }

    json_object *const json = json_tokener_parse(post_state->body);

    if (is_error(json))
    {
        error_msg(_("Unable to parse response from ureport server at '%s'"), config->ur_url);
        VERB2 log("%s", post_state->body);
        json_object_put(json);
        return NULL;
    }

    struct ureport_server_response *response = ureport_server_parse_json(json);
    json_object_put(json);

    if (!response)
        error_msg(_("The response from '%s' has invalid format"), config->ur_url);
    else if ((post_state->http_resp_code == 202 && response->urr_is_error)
                || (post_state->http_resp_code != 202 && !response->urr_is_error))
    {
        /* HTTP CODE 202 means that call was successful but the response */
        /* has an error message */
        error_msg(_("Type mismatch has been detected in the response from '%s'"), config->ur_url);
    }

    return response;
}

bool
ureport_server_response_save_in_dump_dir(struct ureport_server_response *resp,
                                         const char *dump_dir_path,
                                         struct ureport_server_config *config)
{
    struct dump_dir *dd = dd_opendir(dump_dir_path, /* flags */ 0);
    if (!dd)
        return false;

    if (resp->urr_bthash)
    {
        char *msg = xasprintf("uReport: BTHASH=%s", resp->urr_bthash);
        add_reported_to(dd, msg);
        free(msg);

        char *report_url = ureport_server_response_get_report_url(resp, config);
        msg = xasprintf("ABRT Server: URL=%s", report_url);
        add_reported_to(dd, msg);
        free(msg);
        free(report_url);
    }

    if (resp->urr_reported_to_list)
    {
        for (GList *e = resp->urr_reported_to_list; e; e = g_list_next(e))
            add_reported_to(dd, e->data);
    }

    if (resp->urr_solution)
        dd_save_text(dd, FILENAME_NOT_REPORTABLE, resp->urr_solution);

    dd_close(dd);
    return true;
}

char *
ureport_server_response_get_report_url(struct ureport_server_response *resp,
                                       struct ureport_server_config *config)
{
    char *bthash_url = concat_path_file(config->ur_url, BTHASH_URL_SFX);
    char *report_url = concat_path_file(bthash_url, resp->urr_bthash);
    free(bthash_url);
    return report_url;
}

static void
ureport_add_str(struct json_object *ur, const char *key, const char *s)
{
    struct json_object *jstring = json_object_new_string(s);
    if (!jstring)
        die_out_of_memory();

    json_object_object_add(ur, key, jstring);
}

char *
ureport_from_dump_dir_ext(const char *dump_dir_path, const struct ureport_preferences *preferences)
{
    char *error_message;
    struct sr_report *report = sr_abrt_report_from_dir(dump_dir_path,
                                                       &error_message);

    if (!report)
        error_msg_and_die("%s", error_message);

    if (preferences != NULL && preferences->urp_auth_items != NULL)
    {
        struct dump_dir *dd = dd_opendir(dump_dir_path, DD_OPEN_READONLY);
        if (!dd)
            xfunc_die(); /* dd_opendir() already printed an error message */

        GList *iter = preferences->urp_auth_items;
        for ( ; iter != NULL; iter = g_list_next(iter))
        {
            const char *key = (const char *)iter->data;
            char *value = dd_load_text_ext(dd, key,
                    DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE | DD_FAIL_QUIETLY_ENOENT);

            if (value == NULL)
            {
                perror_msg("Cannot include '%s' in 'auth'", key);
                continue;
            }

            sr_report_add_auth(report, key, value);
        }

        dd_close(dd);
    }

    char *json_ureport = sr_report_to_json(report);
    sr_report_free(report);

    return json_ureport;
}

char *
ureport_from_dump_dir(const char *dump_dir_path)
{
    return ureport_from_dump_dir_ext(dump_dir_path, /*no preferences*/NULL);
}

struct abrt_post_state *
ureport_do_post(const char *json, struct ureport_server_config *config,
                const char *url_sfx)
{

    int flags = ABRT_POST_WANT_BODY | ABRT_POST_WANT_ERROR_MSG;

    if (config->ur_ssl_verify)
        flags |= ABRT_POST_WANT_SSL_VERIFY;

    struct abrt_post_state *post_state = new_abrt_post_state(flags);

    if (config->ur_client_cert && config->ur_client_key)
    {
        post_state->client_cert_path = config->ur_client_cert;
        post_state->client_key_path = config->ur_client_key;
    }

    if (config->ur_client_cert && config->ur_client_key)
    {
        post_state->client_cert_path = config->ur_client_cert;
        post_state->client_key_path = config->ur_client_key;
    }
    else if (config->ur_username && config->ur_password)
    {
        post_state->username = config->ur_username;
        post_state->password = config->ur_password;
    }

    char **headers = xmalloc(sizeof(char *) * (3 + size_map_string(config->ur_http_headers)));
    headers[0] = (char *)"Accept: application/json";
    headers[1] = (char *)"Connection: close";
    headers[2] = NULL;

    if (config->ur_http_headers != NULL)
    {
        unsigned i = 2;
        const char *header;
        const char *value;
        map_string_iter_t iter;
        init_map_string_iter(&iter, config->ur_http_headers);
        while (next_map_string_iter(&iter, &header, &value))
            headers[i++] = xasprintf("%s: %s", header, value);
        headers[i] = NULL;
    }

    char *dest_url = concat_path_file(config->ur_url, url_sfx);

    abrt_post_string_as_form_data(post_state, dest_url, "application/json",
                     (const char **)headers, json);

    /* Client authentication failed. Try again without client auth.
     * CURLE_SSL_CONNECT_ERROR - cert not found/server doesnt trust the CA
     * CURLE_SSL_CERTPROBLEM - malformed certificate/no permission
     */
    if ((post_state->curl_result == CURLE_SSL_CONNECT_ERROR
         || post_state->curl_result == CURLE_SSL_CERTPROBLEM)
            && config->ur_client_cert && config->ur_client_key)
    {
        log(_("Authentication failed. Retrying unauthenticated."));
        free_abrt_post_state(post_state);
        post_state = new_abrt_post_state(flags);

        abrt_post_string_as_form_data(post_state, dest_url, "application/json",
                         (const char **)headers, json);
    }

    free(dest_url);

    for (unsigned i = size_map_string(config->ur_http_headers); i != 0; --i)
        free(headers[i + 1]);
    free(headers);

    return post_state;
}

struct ureport_server_response *
ureport_submit(const char *json, struct ureport_server_config *config)
{
    struct abrt_post_state *post_state = ureport_do_post(json, config, UREPORT_SUBMIT_ACTION);

    if (post_state == NULL)
    {
        error_msg(_("Failed on submitting the problem"));
        return NULL;
    }

    struct ureport_server_response *resp = ureport_server_response_from_reply(post_state, config);
    free(post_state);

    return resp;
}

char *
ureport_json_attachment_new(const char *bthash, const char *type, const char *data)
{
    struct json_object *attachment = json_object_new_object();
    if (!attachment)
        die_out_of_memory();

    ureport_add_str(attachment, "bthash", bthash);
    ureport_add_str(attachment, "type", type);
    ureport_add_str(attachment, "data", data);

    char *result = xstrdup(json_object_to_json_string(attachment));
    json_object_put(attachment);

    return result;
}

bool
ureport_attach_string(const char *bthash, const char *type, const char *data,
               struct ureport_server_config *config)
{
    char *json = ureport_json_attachment_new(bthash, type, data);
    struct abrt_post_state *post_state = ureport_do_post(json, config, UREPORT_ATTACH_ACTION);
    free(json);

    struct ureport_server_response *resp =
        ureport_server_response_from_reply(post_state, config);
    free_abrt_post_state(post_state);
    /* don't use str_bo_bool() because we require "true" string */
    const int result = !resp || resp->urr_is_error || strcmp(resp->urr_value, "true") != 0;

    if (resp && resp->urr_is_error)
        error_msg(_("The server at '%s' responded with an error: '%s'"),
                config->ur_url, resp->urr_value);

    ureport_server_response_free(resp);

    return result;
}

bool
ureport_attach_int(const char *bthash, const char *type, int data,
                    struct ureport_server_config *config)
{
    char *data_str = xasprintf("%d", data);
    const bool result = ureport_attach_string(bthash, type, data_str, config);
    free(data_str);

    return result;
}
