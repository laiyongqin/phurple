/**
 * Copyright (c) 2007-2008, Anatoliy Belsky
 *
 * This file is part of PHPurple.
 *
 * PHPhurple is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PHPhurple is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PHPhurple.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <php.h>
#include <php_ini.h>
#include <ext/standard/info.h>

#include <main/php_config.h>
#ifdef HAVE_BUNDLED_PCRE
#include <ext/pcre/pcrelib/pcre.h>
#elif HAVE_PCRE
#include <pcre.h>
#endif

#include "php_phurple.h"

#include <glib.h>

#include <string.h>
#include <ctype.h>

#include <purple.h>

#ifdef HAVE_SIGNAL_H
# include <signal.h>
#include <sys/wait.h>
#endif

#define PHURPLE_GLIB_READ_COND  (G_IO_IN | G_IO_HUP | G_IO_ERR)
#define PHURPLE_GLIB_WRITE_COND (G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL)

ZEND_DECLARE_MODULE_GLOBALS(phurple)

static void phurple_glib_io_destroy(gpointer data);
static gboolean phurple_glib_io_invoke(GIOChannel *source, GIOCondition condition, gpointer data);
static guint glib_input_add(gint fd, PurpleInputCondition condition, PurpleInputFunction function, gpointer data);
static void phurple_write_conv_function(PurpleConversation *conv, const char *who, const char *alias, const char *message, PurpleMessageFlags flags, time_t mtime);
static void phurple_write_im_function(PurpleConversation *conv, const char *who, const char *message, PurpleMessageFlags flags, time_t mtime);
static void phurple_signed_off_function(PurpleConnection *gc, gpointer null);
static void phurple_g_log_handler(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data);
static void phurple_ui_init();
static void *phurple_request_authorize(PurpleAccount *account, const char *remote_user, const char *id, const char *alias, const char *message,
									   gboolean on_list, PurpleAccountRequestAuthorizationCb auth_cb, PurpleAccountRequestAuthorizationCb deny_cb, void *user_data);

#ifdef HAVE_SIGNAL_H
static void sighandler(int sig);
static void clean_pid();

char *segfault_message = "";

static int catch_sig_list[] = {
	SIGSEGV,
	SIGHUP,
	SIGINT,
	SIGTERM,
	SIGQUIT,
	SIGCHLD,
	SIGALRM,
	-1
};

static int ignore_sig_list[] = {
	SIGPIPE,
	-1
};
#endif

typedef struct _PurpleGLibIOClosure {
	PurpleInputFunction function;
	guint result;
	gpointer data;
} PurpleGLibIOClosure;


PurpleEventLoopUiOps glib_eventloops =
{
	g_timeout_add,
	g_source_remove,
	glib_input_add,
	g_source_remove,
	NULL,
#if GLIB_CHECK_VERSION(2,14,0)
	g_timeout_add_seconds,
#else
	NULL,
#endif
	NULL,
	NULL,
	NULL
};


PurpleConversationUiOps php_conv_uiops =
{
	NULL,					  /* create_conversation  */
	NULL,					  /* destroy_conversation */
	NULL,			/* write_chat		   */
	phurple_write_im_function,			  /* write_im			 */
	phurple_write_conv_function,			/* write_conv		   */
	NULL,					  /* chat_add_users	   */
	NULL,					  /* chat_rename_user	 */
	NULL,					  /* chat_remove_users	*/
	NULL,					  /* chat_update_user	 */
	NULL,					  /* present			  */
	NULL,					  /* has_focus			*/
	NULL,					  /* custom_smiley_add	*/
	NULL,					  /* custom_smiley_write  */
	NULL,					  /* custom_smiley_close  */
	NULL,					  /* send_confirm		 */
	NULL,
	NULL,
	NULL,
	NULL
};


PurpleCoreUiOps php_core_uiops =
{
	NULL,
	NULL,
	phurple_ui_init,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};


PurpleAccountUiOps php_account_uiops = 
{
	NULL,				/* notify added */
	NULL,				/* status changed */
	NULL,				/* request add */
	phurple_request_authorize,				/* request authorize */
	NULL,				/* close account request */
	NULL,
	NULL,
	NULL,
	NULL
};


/* classes definitions*/
zend_class_entry *PhurpleClient_ce, *PhurpleConversation_ce, *PhurpleAccount_ce, *PhurpleConnection_ce, *PhurpleBuddy_ce, *PhurpleBuddyList_ce, *PhurpleBuddyGroup_ce;


void phurple_globals_ctor(zend_phurple_globals *phurple_globals TSRMLS_DC)
{
	/*MAKE_STD_ZVAL(phurple_globals->phurple_client_obj);*/
	/*ALLOC_INIT_ZVAL(phurple_globals->phurple_client_obj);
	Z_TYPE_P(phurple_globals->phurple_client_obj) = IS_OBJECT;*/
	phurple_globals->phurple_client_obj = NULL;

	zend_hash_init(&(phurple_globals->ppos).buddy, 32, NULL, NULL, 0);
	zend_hash_init(&(phurple_globals->ppos).group, 32, NULL, NULL, 0);

	phurple_globals->debug = 0;
	phurple_globals->custom_user_dir = estrdup("/dev/null");
	phurple_globals->custom_plugin_path = estrdup("");
	phurple_globals->ui_id = estrdup("PHP");
}

void phurple_globals_dtor(zend_phurple_globals *phurple_globals TSRMLS_DC)
{
	if(NULL != phurple_globals->phurple_client_obj) {
		zval_ptr_dtor(&phurple_globals->phurple_client_obj);
	}

	/*zend_hash_destroy(&(phurple_globals->ppos).buddy);
	zend_hash_destroy(&(phurple_globals->ppos).group);

	efree(phurple_globals->custom_user_dir);
	efree(phurple_globals->custom_plugin_path);
	efree(phurple_globals->ui_id);*/
}

/* True global resources - no need for thread safety here */
static int le_phurple;

/* {{{ phurple_functions[] */
zend_function_entry phurple_functions[] = {
	{NULL, NULL, NULL}
};
/* }}} */

/* {{{ client class methods[] */
zend_function_entry PhurpleClient_methods[] = {
	PHP_ME(PhurpleClient, __construct, NULL, ZEND_ACC_FINAL | ZEND_ACC_PROTECTED)
	PHP_ME(PhurpleClient, getInstance, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(PhurpleClient, initInternal, NULL, ZEND_ACC_PROTECTED)
	PHP_ME(PhurpleClient, getCoreVersion, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleClient, writeConv, NULL, ZEND_ACC_PROTECTED)
	PHP_ME(PhurpleClient, writeIM, NULL, ZEND_ACC_PROTECTED)
	PHP_ME(PhurpleClient, onSignedOn, NULL, ZEND_ACC_PROTECTED)
	/*PHP_ME(PhurpleClient, onSignedOff, NULL, ZEND_ACC_PROTECTED)*/
	PHP_ME(PhurpleClient, runLoop, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleClient, addAccount, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleClient, getProtocols, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleClient, loopCallback, NULL, ZEND_ACC_PROTECTED)
	PHP_ME(PhurpleClient, loopHeartBeat, NULL, ZEND_ACC_PROTECTED)
	PHP_ME(PhurpleClient, deleteAccount, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleClient, findAccount, NULL, ZEND_ACC_PUBLIC )
	PHP_ME(PhurpleClient, authorizeRequest, NULL, ZEND_ACC_PROTECTED)
	PHP_ME(PhurpleClient, iterate, NULL, ZEND_ACC_PUBLIC)
	/*PHP_ME(PhurpleClient, set, NULL, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(PhurpleClient, get, NULL, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)*/
	PHP_ME(PhurpleClient, connect, NULL, ZEND_ACC_PUBLIC)
	/*PHP_ME(PhurpleClient, disconnect, NULL, ZEND_ACC_PUBLIC)*/
	PHP_ME(PhurpleClient, setUserDir, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(PhurpleClient, setDebug, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(PhurpleClient, setUiId, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(PhurpleClient, __clone, NULL, ZEND_ACC_FINAL | ZEND_ACC_PRIVATE)
	{NULL, NULL, NULL}
};
/* }}} */


/* {{{ conversation class methods[] */
zend_function_entry PhurpleConversation_methods[] = {
	PHP_ME(PhurpleConversation, __construct, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleConversation, getName, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleConversation, sendIM, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleConversation, getAccount, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleConversation, setAccount, NULL, ZEND_ACC_PUBLIC)
	{NULL, NULL, NULL}
};
/* }}} */


/* {{{ account class methods[] */
zend_function_entry PhurpleAccount_methods[] = {
	PHP_ME(PhurpleAccount, __construct, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleAccount, setPassword, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleAccount, setEnabled, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleAccount, addBuddy, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleAccount, removeBuddy, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleAccount, clearSettings, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleAccount, set, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleAccount, isConnected, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleAccount, isConnecting, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleAccount, getUserName, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleAccount, getPassword, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleAccount, get, NULL, ZEND_ACC_PUBLIC)
	{NULL, NULL, NULL}
};
/* }}} */


/* {{{ connection class methods[] */
zend_function_entry PhurpleConnection_methods[] = {
	PHP_ME(PhurpleConnection, __construct, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleConnection, getAccount, NULL, ZEND_ACC_PUBLIC)
	{NULL, NULL, NULL}
};
/* }}} */


/* {{{ buddy class methods[] */
zend_function_entry PhurpleBuddy_methods[] = {
	PHP_ME(PhurpleBuddy, __construct, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleBuddy, getName, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleBuddy, getAlias, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleBuddy, getGroup, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleBuddy, getAccount, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleBuddy, isOnline, NULL, ZEND_ACC_PUBLIC)
	{NULL, NULL, NULL}
};
/* }}} */


/* {{{ buddy list class methods[] */
zend_function_entry PhurpleBuddyList_methods[] = {
	PHP_ME(PhurpleBuddyList, __construct, NULL, ZEND_ACC_PRIVATE)
	PHP_ME(PhurpleBuddyList, addBuddy, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(PhurpleBuddyList, addGroup, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(PhurpleBuddyList, findBuddy, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(PhurpleBuddyList, load, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(PhurpleBuddyList, findGroup, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(PhurpleBuddyList, removeBuddy, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(PhurpleBuddyList, removeGroup, NULL, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	{NULL, NULL, NULL}
};
/* }}} */


/* {{{ buddy group class methods[] */
zend_function_entry PhurpleBuddyGroup_methods[] = {
	PHP_ME(PhurpleBuddyGroup, __construct, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleBuddyGroup, getAccounts, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleBuddyGroup, getSize, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleBuddyGroup, getOnlineCount, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(PhurpleBuddyGroup, getName, NULL, ZEND_ACC_PUBLIC)
	{NULL, NULL, NULL}
};
/* }}} */


/* {{{ phurple_module_entry */
zend_module_entry phurple_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"phurple",
	phurple_functions,
	PHP_MINIT(phurple),
	PHP_MSHUTDOWN(phurple),
	PHP_RINIT(phurple),
	PHP_RSHUTDOWN(phurple),
	PHP_MINFO(phurple),
#if ZEND_MODULE_API_NO >= 20010901
	"0.4",
#endif
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_PHURPLE
ZEND_GET_MODULE(phurple)
#endif

/* {{{ PHP_INI */
PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("phurple.custom_plugin_path", "", PHP_INI_ALL, OnUpdateString, custom_plugin_path, zend_phurple_globals, phurple_globals)
PHP_INI_END()
/* }}} */


/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(phurple)
{
	ZEND_INIT_MODULE_GLOBALS(phurple, phurple_globals_ctor, phurple_globals_dtor);
	
	REGISTER_INI_ENTRIES();

	g_log_set_handler (NULL, G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL | G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_RECURSION, phurple_g_log_handler, NULL);
	
	/* initalizing classes */
	zend_class_entry ce;
	
	/* classes definitions */
	INIT_CLASS_ENTRY(ce, PHURPLE_CLIENT_CLASS_NAME, PhurpleClient_methods);
	PhurpleClient_ce = zend_register_internal_class(&ce TSRMLS_CC);

	/* A type of conversation */
	zend_declare_class_constant_long(PhurpleClient_ce, "CONV_TYPE_UNKNOWN", sizeof("CONV_TYPE_UNKNOWN")-1, PURPLE_CONV_TYPE_UNKNOWN TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "CONV_TYPE_IM", sizeof("CONV_TYPE_IM")-1, PURPLE_CONV_TYPE_IM TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "CONV_TYPE_CHAT", sizeof("CONV_TYPE_CHAT")-1, PURPLE_CONV_TYPE_CHAT TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "CONV_TYPE_MISC", sizeof("CONV_TYPE_MISC")-1, PURPLE_CONV_TYPE_MISC TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "CONV_TYPE_ANY", sizeof("CONV_TYPE_ANY")-1, PURPLE_CONV_TYPE_ANY TSRMLS_CC);
	/* Flags applicable to a message */
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_SEND", sizeof("MESSAGE_SEND")-1, PURPLE_MESSAGE_SEND TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_RECV", sizeof("MESSAGE_RECV")-1, PURPLE_MESSAGE_RECV TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_SYSTEM", sizeof("MESSAGE_SYSTEM")-1, PURPLE_MESSAGE_SYSTEM TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_AUTO_RESP", sizeof("MESSAGE_AUTO_RESP")-1, PURPLE_MESSAGE_AUTO_RESP TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_ACTIVE_ONLY", sizeof("MESSAGE_ACTIVE_ONLY")-1, PURPLE_MESSAGE_ACTIVE_ONLY TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_NICK", sizeof("MESSAGE_NICK")-1, PURPLE_MESSAGE_NICK TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_NO_LOG", sizeof("MESSAGE_NO_LOG")-1, PURPLE_MESSAGE_NO_LOG TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_WHISPER", sizeof("MESSAGE_WHISPER")-1, PURPLE_MESSAGE_WHISPER TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_ERROR", sizeof("MESSAGE_ERROR")-1, PURPLE_MESSAGE_ERROR TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_DELAYED", sizeof("MESSAGE_DELAYED")-1, PURPLE_MESSAGE_DELAYED TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_RAW", sizeof("MESSAGE_RAW")-1, PURPLE_MESSAGE_RAW TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_IMAGES", sizeof("MESSAGE_IMAGES")-1, PURPLE_MESSAGE_IMAGES TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_NOTIFY", sizeof("MESSAGE_NOTIFY")-1, PURPLE_MESSAGE_NOTIFY TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_NO_LINKIFY", sizeof("MESSAGE_NO_LINKIFY")-1, PURPLE_MESSAGE_NO_LINKIFY TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "MESSAGE_INVISIBLE", sizeof("MESSAGE_INVISIBLE")-1, PURPLE_MESSAGE_INVISIBLE TSRMLS_CC);
	/* Flags applicable to a status */
	zend_declare_class_constant_long(PhurpleClient_ce, "STATUS_OFFLINE", sizeof("STATUS_OFFLINE")-1, PURPLE_STATUS_OFFLINE TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "STATUS_AVAILABLE", sizeof("STATUS_AVAILABLE")-1, PURPLE_STATUS_AVAILABLE TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "STATUS_UNAVAILABLE", sizeof("STATUS_UNAVAILABLE")-1, PURPLE_STATUS_UNAVAILABLE TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "STATUS_INVISIBLE", sizeof("STATUS_INVISIBLE")-1, PURPLE_STATUS_INVISIBLE TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "STATUS_AWAY", sizeof("STATUS_AWAY")-1, PURPLE_STATUS_AWAY TSRMLS_CC);
	zend_declare_class_constant_long(PhurpleClient_ce, "STATUS_MOBILE", sizeof("STATUS_MOBILE")-1, PURPLE_STATUS_MOBILE TSRMLS_CC);
	
	INIT_CLASS_ENTRY(ce, PHURPLE_CONVERSATION_CLASS_NAME, PhurpleConversation_methods);
	PhurpleConversation_ce = zend_register_internal_class(&ce TSRMLS_CC);
	zend_declare_property_long(PhurpleConversation_ce, "index", sizeof("index")-1, -1, ZEND_ACC_PRIVATE TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, PHURPLE_ACCOUNT_CLASS_NAME, PhurpleAccount_methods);
	PhurpleAccount_ce = zend_register_internal_class(&ce TSRMLS_CC);
	zend_declare_property_long(PhurpleAccount_ce, "index", sizeof("index")-1, -1, ZEND_ACC_FINAL | ZEND_ACC_PRIVATE TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, PHURPLE_CONNECION_CLASS_NAME, PhurpleConnection_methods);
	PhurpleConnection_ce = zend_register_internal_class(&ce TSRMLS_CC);
	zend_declare_property_long(PhurpleConnection_ce, "index", sizeof("index")-1, -1, ZEND_ACC_FINAL | ZEND_ACC_PRIVATE TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, PHURPLE_BUDDY_CLASS_NAME, PhurpleBuddy_methods);
	PhurpleBuddy_ce = zend_register_internal_class(&ce TSRMLS_CC);
	zend_declare_property_long(PhurpleBuddy_ce, "index", sizeof("index")-1, -1, ZEND_ACC_FINAL | ZEND_ACC_PRIVATE TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, PHURPLE_BUDDYLIST_CLASS_NAME, PhurpleBuddyList_methods);
	PhurpleBuddyList_ce = zend_register_internal_class(&ce TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, PHURPLE_BUDDY_GROUP_CLASS_NAME, PhurpleBuddyGroup_methods);
	PhurpleBuddyGroup_ce = zend_register_internal_class(&ce TSRMLS_CC);

	
	/* end initalizing classes */
	
#ifdef HAVE_SIGNAL_H
	int sig_indx;	/* for setting up signal catching */
	sigset_t sigset;
	RETSIGTYPE (*prev_sig_disp)(int);
	char errmsg[BUFSIZ];

	if (sigemptyset(&sigset)) {
		snprintf(errmsg, BUFSIZ, "Warning: couldn't initialise empty signal set");
		perror(errmsg);
	}
	for(sig_indx = 0; catch_sig_list[sig_indx] != -1; ++sig_indx) {
		if((prev_sig_disp = signal(catch_sig_list[sig_indx], sighandler)) == SIG_ERR) {
			snprintf(errmsg, BUFSIZ, "Warning: couldn't set signal %d for catching",
				catch_sig_list[sig_indx]);
			perror(errmsg);
		}
		if(sigaddset(&sigset, catch_sig_list[sig_indx])) {
			snprintf(errmsg, BUFSIZ, "Warning: couldn't include signal %d for unblocking",
				catch_sig_list[sig_indx]);
			perror(errmsg);
		}
	}
	for(sig_indx = 0; ignore_sig_list[sig_indx] != -1; ++sig_indx) {
		if((prev_sig_disp = signal(ignore_sig_list[sig_indx], SIG_IGN)) == SIG_ERR) {
			snprintf(errmsg, BUFSIZ, "Warning: couldn't set signal %d to ignore",
				ignore_sig_list[sig_indx]);
			perror(errmsg);
		}
	}

	if (sigprocmask(SIG_UNBLOCK, &sigset, NULL)) {
		snprintf(errmsg, BUFSIZ, "Warning: couldn't unblock signals");
		perror(errmsg);
	}
#endif
	
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(phurple)
{
	UNREGISTER_INI_ENTRIES();

#ifdef ZTS
	ts_free_id(phurple_globals_id);
#else
	phurple_globals_dtor(&phurple_globals TSRMLS_CC);
#endif
	
	return SUCCESS;
}
/* }}} */


/* {{{ PHP_RINIT_FUNCTION */
PHP_RINIT_FUNCTION(phurple)
{
	return SUCCESS;
}
/* }}} */


/* {{{ PHP_RSHUTDOWN_FUNCTION */
PHP_RSHUTDOWN_FUNCTION(phurple)
{
	return SUCCESS;
}
/* }}} */


/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(phurple)
{
/*	php_info_print_table_start();
	php_info_print_table_header(2, "phurple support", "enabled");
	php_info_print_table_end();
*/
	
	DISPLAY_INI_ENTRIES();

}
/* }}} */


#if PHURPLE_INTERNAL_DEBUG
void
phurple_dump_zval(zval *var)
{

TSRMLS_FETCH();

	switch (Z_TYPE_P(var)) {
		case IS_NULL:
			php_printf("NULL ");
			break;
		case IS_BOOL:
			php_printf("Boolean: %s ", Z_LVAL_P(var) ? "TRUE" : "FALSE");
			break;
		case IS_LONG:
			php_printf("Long: %ld ", Z_LVAL_P(var));
			break;
		case IS_DOUBLE:
			php_printf("Double: %f ", Z_DVAL_P(var));
			break;
		case IS_STRING:
			php_printf("String: ");
			PHPWRITE(Z_STRVAL_P(var), Z_STRLEN_P(var));
			php_printf(" ");
			break;
		case IS_RESOURCE:
			php_printf("Resource ");
			break;
		case IS_ARRAY:
			php_printf("Array ");
			break;
		case IS_OBJECT:
			php_printf("Object ");
			break;
		default:
			php_printf("Unknown ");
	}
}
#endif


/* {{{ */
static void
phurple_ui_init()
{
	purple_conversations_set_ui_ops(&php_conv_uiops);
}
/* }}} */


/* {{{ just took this two functions from the readline extension */
zval*
phurple_string_zval(const char *str)
{
	zval *ret;
	
	MAKE_STD_ZVAL(ret);
	
	if ((char*)str) {
		ZVAL_STRING(ret, (char*)str, 1);
	} else {
		ZVAL_NULL(ret);
	}

	return ret;
}
/* }}} */


/* {{{ */
zval*
phurple_long_zval(long l)
{
	zval *ret;
	MAKE_STD_ZVAL(ret);

	Z_TYPE_P(ret) = IS_LONG;
	Z_LVAL_P(ret) = l;

	return ret;
}
/* }}} */


/* {{{ */
char*
phurple_tolower(const char *s)
{
	int  i = 0;
	char *r = estrdup(s);

	while (r[i])
	{
		r[i] = tolower(r[i]);
		i++;
	}

	return r;
}
/* }}} */


/* {{{ */
int
phurple_hash_index_find(HashTable *ht, void *element)
{
	ulong i;

	for(i=0; i<zend_hash_num_elements(ht); i++) {
		if(zend_hash_index_find(ht, i, &element) != FAILURE) {
			return (int)i;
		}
	}

	return FAILURE;
}
/* }}} */


/* {{{ */
char*
phurple_get_protocol_id_by_name(const char *protocol_name)
{
	GList *iter;

	iter = purple_plugins_get_protocols();

	for (; iter; iter = iter->next) {
		PurplePlugin *plugin = iter->data;
		PurplePluginInfo *info = plugin->info;
		if (info && info->name && 0 == strcmp(phurple_tolower(info->name), phurple_tolower(protocol_name))) {
			return estrdup(info->id);
		}
	}

	return "";
}
/* }}} */


/* {{{
 Only returns the returned zval if retval_ptr != NULL */
zval*
call_custom_method(zval **object_pp, zend_class_entry *obj_ce, zend_function **fn_proxy, char *function_name, int function_name_len, zval **retval_ptr_ptr, int param_count, ... )
{
	int result, i;
	zend_fcall_info fci;
	zval z_fname, ***params, *retval;
	HashTable *function_table;
	va_list given_params;
	zend_fcall_info_cache fcic;
		/**
		 * TODO Remove this call and pass the tsrm_ls directly as param
		 */
	TSRMLS_FETCH();

#if PHURPLE_INTERNAL_DEBUG
	php_printf("==================== call_custom_method begin ============================\n");
	php_printf("class: %s\n", obj_ce->name);
	php_printf("method name: %s\n", function_name);
#endif

	params = (zval ***) safe_emalloc(param_count, sizeof(zval **), 0);

	va_start(given_params, param_count);

#if PHURPLE_INTERNAL_DEBUG
	php_printf("param count: %d\n", param_count);
#endif
	for(i=0;i<param_count;i++) {
		params[i] = va_arg(given_params, zval **);
#if PHURPLE_INTERNAL_DEBUG
		php_printf("i=>%d: ", i);phurple_dump_zval(*params[i]);php_printf("\n");
#endif
	}
	va_end(given_params);
	
	fci.size = sizeof(fci);
#if !PHURPLE_USING_PHP_53
	fci.object_pp = object_pp;
#endif
	fci.function_name = &z_fname;
	fci.retval_ptr_ptr = retval_ptr_ptr ? retval_ptr_ptr : &retval;
	fci.param_count = param_count;
	fci.params = params;
	fci.no_separation = 1;
	fci.symbol_table = NULL;

	if (!fn_proxy && !obj_ce) {
		/* no interest in caching and no information already present that is
		 * needed later inside zend_call_function. */
		ZVAL_STRINGL(&z_fname, function_name, function_name_len, 0);
		fci.function_table = !object_pp ? EG(function_table) : NULL;
		result = zend_call_function(&fci, NULL TSRMLS_CC);
	} else {
		fcic.initialized = 1;
		if (!obj_ce) {
			obj_ce = object_pp ? Z_OBJCE_PP(object_pp) : NULL;
		}
		if (obj_ce) {
			function_table = &obj_ce->function_table;
		} else {
			function_table = EG(function_table);
		}
		if (!fn_proxy || !*fn_proxy) {
			if (zend_hash_find(function_table, function_name, function_name_len+1, (void **) &fcic.function_handler) == FAILURE) {
				/* error at c-level */
				zend_error(E_CORE_ERROR, "Couldn't find implementation for method %s%s%s", obj_ce ? obj_ce->name : "", obj_ce ? "::" : "", function_name);
			}
			if (fn_proxy) {
				*fn_proxy = fcic.function_handler;
			}
		} else {
			fcic.function_handler = *fn_proxy;
		}
		fcic.calling_scope = obj_ce;
#if PHURPLE_USING_PHP_53
	fcic.object_ptr = *object_pp;
#endif

		result = zend_call_function(&fci, &fcic TSRMLS_CC);
	}

	if (result == FAILURE) {
		/* error at c-level */
		if (!obj_ce) {
			obj_ce = object_pp ? Z_OBJCE_PP(object_pp) : NULL;
		}
		if (!EG(exception)) {
			zend_error(E_CORE_ERROR, "Couldn't execute method %s%s%s", obj_ce ? obj_ce->name : "", obj_ce ? "::" : "", function_name);
		}
	}

	if(params) {
		efree(params);
	}
#if PHURPLE_INTERNAL_DEBUG
	php_printf("==================== call_custom_method end ============================\n\n");
#endif
	if (!retval_ptr_ptr) {
		if (retval) {
			zval_ptr_dtor(&retval);
		}
		return NULL;
	}

	return *retval_ptr_ptr;
}
/* }}} */


/* {{{ */
static void*
phurple_request_authorize(PurpleAccount *account,
							 const char *remote_user,
							 const char *id,
							 const char *alias,
							 const char *message,
							 gboolean on_list,
							 PurpleAccountRequestAuthorizationCb auth_cb,
							 PurpleAccountRequestAuthorizationCb deny_cb,
							 void *user_data)
{
	TSRMLS_FETCH();

	zval *client = PHURPLE_G(phurple_client_obj);
	zend_class_entry *ce = Z_OBJCE_P(client);
	
	zval *result, *php_account, *php_on_list, *php_remote_user, *php_message;
	
	if(NULL != account) {
		ALLOC_INIT_ZVAL(php_account);
		Z_TYPE_P(php_account) = IS_OBJECT;
		object_init_ex(php_account, PhurpleAccount_ce);
		zend_update_property_long(PhurpleAccount_ce,
								  php_account,
								  "index",
								  sizeof("index")-1,
								  (long)g_list_position(purple_accounts_get_all(), g_list_find(purple_accounts_get_all(), account)) TSRMLS_CC
								  );
	} else {
		ALLOC_INIT_ZVAL(php_account);
	}
	
	MAKE_STD_ZVAL(php_on_list);
	ZVAL_BOOL(php_on_list, (long)on_list);
	
	php_message = phurple_string_zval(message);
	php_remote_user = phurple_string_zval(remote_user);
	
	call_custom_method(&client,
					   ce,
					   NULL,
					   "authorizerequest",
					   sizeof("authorizerequest")-1,
					   &result,
					   4,
					   &php_account,
					   &php_remote_user,
					   &php_message,
					   &php_on_list
					   );
	
	if(Z_TYPE_P(result) == IS_BOOL || Z_TYPE_P(result) == IS_LONG || Z_TYPE_P(result) == IS_DOUBLE) {
		if((gboolean) Z_LVAL_P(result)) {
			auth_cb(user_data);
		} else {
			deny_cb(user_data);
		}
		
	}
}
/* }}} */


#ifdef HAVE_SIGNAL_H
/* {{{ */
static void
clean_pid()
{
	int status;
	pid_t pid;

	do {
		pid = waitpid(-1, &status, WNOHANG);
	} while (pid != 0 && pid != (pid_t)-1);

	if ((pid == (pid_t) - 1) && (errno != ECHILD)) {
		char errmsg[BUFSIZ];
		snprintf(errmsg, BUFSIZ, "Warning: waitpid() returned %d", pid);
		perror(errmsg);
	}

	/* Restore signal catching */
	signal(SIGALRM, sighandler);
}
/* }}} */


/* {{{ */
static void
sighandler(int sig)
{
	switch (sig) {
	case SIGHUP:
		purple_debug_warning("sighandler", "Caught signal %d\n", sig);
		purple_connections_disconnect_all();
		break;
	case SIGSEGV:
		fprintf(stderr, "%s", segfault_message);
		abort();
		break;
	case SIGCHLD:
		/* Restore signal catching */
		signal(SIGCHLD, sighandler);
		alarm(1);
		break;
	case SIGALRM:
		clean_pid();
		break;
	default:
		purple_debug_warning("sighandler", "Caught signal %d\n", sig);
		
		purple_connections_disconnect_all();

		purple_plugins_unload_all();

		exit(0);
	}
}
/* }}} */
#endif

/* {{{ */
static void
phurple_glib_io_destroy(gpointer data)
{
	g_free(data);
}
/* }}} */


/* {{{ */
static gboolean
phurple_glib_io_invoke(GIOChannel *source, GIOCondition condition, gpointer data)
{
	PurpleGLibIOClosure *closure = data;
	PurpleInputCondition purple_cond = 0;
	
	if(condition & PHURPLE_GLIB_READ_COND) {
		purple_cond |= PURPLE_INPUT_READ;
	}
	
	if(condition & PHURPLE_GLIB_WRITE_COND) {
		purple_cond |= PURPLE_INPUT_WRITE;
	}
	
	closure->function(closure->data, g_io_channel_unix_get_fd(source), purple_cond);
	
	return TRUE;
}
/* }}} */


/* {{{ */
static guint
glib_input_add(gint fd, PurpleInputCondition condition, PurpleInputFunction function,
							   gpointer data)
{
	PurpleGLibIOClosure *closure = g_new0(PurpleGLibIOClosure, 1);
	GIOChannel *channel;
	GIOCondition cond = 0;
	
	closure->function = function;
	closure->data = data;
	
	if (condition & PURPLE_INPUT_READ) {
		cond |= PHURPLE_GLIB_READ_COND;
	}
	
	if (condition & PURPLE_INPUT_WRITE) {
		cond |= PHURPLE_GLIB_WRITE_COND;
	}
	
	channel = g_io_channel_unix_new(fd);
	closure->result = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT, cond,
										  phurple_glib_io_invoke, closure, phurple_glib_io_destroy);
	
	g_io_channel_unref(channel);
	return closure->result;
}
/* }}} */


/* {{{ */
static void
phurple_write_conv_function(PurpleConversation *conv, const char *who, const char *alias, const char *message, PurpleMessageFlags flags, time_t mtime)
{
	/**
	 * Just mirroring phurple_write_im_function despite we
	 * loose the alias to just not to implement the same twice
	 */
	phurple_write_im_function(conv, who, message, flags, mtime);
}
/* }}} */


/* {{{ */
static void
phurple_write_im_function(PurpleConversation *conv, const char *who, const char *message, PurpleMessageFlags flags, time_t mtime)
{
	const int PARAMS_COUNT = 5;
	zval ***params, *conversation, *buddy, *datetime, *retval, *tmp1, *tmp2, *tmp3;
	GList *conversations = purple_get_conversations();
	PurpleBuddy *pbuddy = NULL;
	PurpleAccount *paccount = NULL;

	char *who_san = (!who || '\0' == who) ? "" : (char*)who;
	char *message_san = (!message || '\0' == message) ? "" : (char*)message;

	TSRMLS_FETCH();

	PHURPLE_MK_OBJ(conversation, PhurpleConversation_ce);
	zend_update_property_long(PhurpleConversation_ce,
							  conversation,
							  "index",
							  sizeof("index")-1,
							  (long)g_list_position(conversations, g_list_find(conversations, conv)) TSRMLS_CC
							  );

	zval *client = PHURPLE_G(phurple_client_obj);
	zend_class_entry *ce = Z_OBJCE_P(client);

	paccount = g_list_nth_data (purple_accounts_get_all(), g_list_position(purple_accounts_get_all(),g_list_find(purple_accounts_get_all(), (gconstpointer)purple_conversation_get_account(conv))));
	if(paccount) {
		pbuddy = purple_find_buddy(paccount, !who_san ? purple_conversation_get_name(conv) : who_san);
		
		if(NULL != pbuddy) {
			int ind = phurple_hash_index_find(&(PHURPLE_G(ppos).buddy), pbuddy);
			PHURPLE_MK_OBJ(buddy, PhurpleBuddy_ce);
			
			if(ind == FAILURE) {
				ulong nextid = zend_hash_next_free_element(&(PHURPLE_G(ppos).buddy));
				zend_hash_index_update(&(PHURPLE_G(ppos).buddy), nextid, (void*)pbuddy, sizeof(PurpleBuddy*), NULL);

				zend_update_property_long(PhurpleBuddy_ce,
										  buddy,
										  "index",
										  sizeof("index")-1,
										  (long)nextid TSRMLS_CC
										  );
			} else {
				zend_update_property_long(PhurpleBuddy_ce,
										  buddy,
										  "index",
										  sizeof("index")-1,
										  (long)ind TSRMLS_CC
										 );
			}
		} else {
			if(who_san) {
				buddy = phurple_string_zval(who_san);
			} else {
				ALLOC_INIT_ZVAL(buddy);
			}
		}
	}

	tmp1 = phurple_string_zval(message_san);
	tmp2 = phurple_long_zval((long)flags);
	tmp3 = phurple_long_zval((long)mtime);

	call_custom_method(&client,
					   ce,
					   NULL,
					   "writeim",
					   sizeof("writeim")-1,
					   NULL,
					   PARAMS_COUNT,
					   &conversation,
					   &buddy,
					   &tmp1,
					   &tmp2,
					   &tmp3
					   );

	zval_ptr_dtor(&tmp1);
	zval_ptr_dtor(&tmp2);
	zval_ptr_dtor(&tmp3);
	zval_ptr_dtor(&conversation);

}
/* }}} */


/* {{{ */
static void
phurple_g_log_handler(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data)
{
	/**
	 * @todo put here some php callback
	 */
}
/* }}} */


/* {{{ */
static void
phurple_signed_off_function(PurpleConnection *conn, gpointer null)
{
	zval *connection, *retval;
	GList *connections = NULL;

	TSRMLS_FETCH();

	zval *client = PHURPLE_G(phurple_client_obj);
	zend_class_entry *ce = Z_OBJCE_P(client);
	
	connections = purple_connections_get_all();

	PHURPLE_MK_OBJ(connection, PhurpleConnection_ce);
	zend_update_property_long(PhurpleConnection_ce,
							  connection,
							  "index",
							  sizeof("index")-1,
							  (long)g_list_position(connections, g_list_find(connections, conn)) TSRMLS_CC
							  );

	call_custom_method(&client,
					   ce,
					   NULL,
					   "onsignedoff",
					   sizeof("onsignedoff")-1,
					   NULL,
					   1,
					   &connection);
	
	zval_ptr_dtor(&connection);
}
/* }}} */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */