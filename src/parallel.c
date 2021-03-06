/*
  +----------------------------------------------------------------------+
  | parallel                                                              |
  +----------------------------------------------------------------------+
  | Copyright (c) Joe Watkins 2019                                       |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: krakjoe                                                      |
  +----------------------------------------------------------------------+
 */
#ifndef HAVE_PARALLEL_PARALLEL
#define HAVE_PARALLEL_PARALLEL

#include "parallel.h"
#include "future.h"
#include "copy.h"

#include "php_main.h"
#include "SAPI.h"
#include "TSRM.h"

#include "zend_closures.h"
#include "zend_exceptions.h"

typedef int (*php_sapi_deactivate_t)(void);

php_sapi_deactivate_t php_sapi_deactivate_function;

zend_class_entry *php_parallel_exception_ce;
zend_class_entry *php_parallel_ce;
zend_object_handlers php_parallel_handlers;

zend_string *php_parallel_main;

void* php_parallel_routine(void *arg);

TSRM_TLS php_parallel_t *context = NULL;

void (*zend_interrupt_handler)(zend_execute_data*) = NULL;

void php_parallel_interrupt(zend_execute_data *execute_data) {
	if (context && 
	    php_parallel_monitor_check(context->monitor, PHP_PARALLEL_KILLED)) {
		zend_bailout();
	}

	if (zend_interrupt_handler) {
		zend_interrupt_handler(execute_data);
	}
}

void php_parallel_execute(php_parallel_monitor_t *monitor, zend_function *function, zval *argv, zval *retval) {
	zval rv;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	int rc = FAILURE;
    zval args;
    
	fci.size = sizeof(zend_fcall_info);
	fci.retval = &rv;
#if PHP_VERSION_ID < 70300
	fcc.initialized = 1;
#endif

	fcc.function_handler = php_parallel_copy(function, 0);

	if (!Z_ISUNDEF_P(argv)) {
	    php_parallel_copy_zval(
	        &args, argv, 0);
	    
		zend_fcall_info_args(&fci, &args);
	}

	ZVAL_UNDEF(&rv);

	zend_try {
		rc = zend_call_function(&fci, &fcc);
	} zend_catch {
		if (monitor) {
			if (php_parallel_monitor_check(context->monitor, PHP_PARALLEL_KILLED)) {
				php_parallel_monitor_set(monitor, 
					PHP_PARALLEL_KILLED|PHP_PARALLEL_ERROR, 0);
			} else {
				php_parallel_monitor_set(monitor, PHP_PARALLEL_ERROR, 0);
			}
		}
	} zend_end_try();

	if (rc == SUCCESS && !Z_ISUNDEF(rv)) {
		if (retval) {
			php_parallel_copy_zval(retval, &rv, 1);
		}

		if (Z_REFCOUNTED(rv)) {
			zval_ptr_dtor(&rv);
		}
	}

	if (!Z_ISUNDEF_P(argv)) {
		zend_fcall_info_args_clear(
		    &fci, 1);
		
		php_parallel_zval_dtor(&args);
	}

	php_parallel_copy_free(fcc.function_handler, 0);
}

static zend_always_inline void php_parallel_configure_callback(int (*zend_callback) (char *, size_t), zval *value) {
	if (Z_TYPE_P(value) == IS_ARRAY) {
		zval *val;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(value), val) {
			if (Z_TYPE_P(val) == IS_STRING) {
				zend_callback(Z_STRVAL_P(val), Z_STRLEN_P(val));
			}
		} ZEND_HASH_FOREACH_END();
	} else if (Z_TYPE_P(value) == IS_STRING) {
		char *start  = Z_STRVAL_P(value),
		     *end    = Z_STRVAL_P(value) + Z_STRLEN_P(value),
		     *next   = (char *) php_memnstr(Z_STRVAL_P(value), ZEND_STRL(","), end);

		if (next == NULL) {
			zend_callback(Z_STRVAL_P(value), Z_STRLEN_P(value));
			return;
		}

		do {
			zend_callback(start, next - start);
			start = next + 1;
			next  = (char *) php_memnstr(start, ZEND_STRL(","), end);
		} while(next);

		if (start <= end) {
			zend_callback(start, end - start);
		}
	}
}

void php_parallel_configure(zval *configuration) {
	zend_string *name;
	zval        *value;

	ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(configuration), name, value) {
		zend_string *chars;
		zend_string *local = zend_string_dup(name, 1);

		if (zend_string_equals_literal_ci(local, "disable_functions")) {
			php_parallel_configure_callback(zend_disable_function, value);
		} else if (zend_string_equals_literal_ci(local, "disable_classes")) {
			php_parallel_configure_callback(zend_disable_class, value);
		} else if (zend_string_equals_literal_ci(local, "extension") ||
			   zend_string_equals_literal_ci(local, "zend_extension")) {
			/* nothing, use dl for modules and don't load zend_extensions */
		} else {
			switch (Z_TYPE_P(value)) {
				case IS_STRING:
				case IS_TRUE:
				case IS_FALSE:
				case IS_LONG:
				case IS_DOUBLE:
					chars = zval_get_string(value);
				break;

				default:
					continue;
			}

			zend_alter_ini_entry_chars(local, 
				ZSTR_VAL(chars), ZSTR_LEN(chars), 
				ZEND_INI_SYSTEM, ZEND_INI_STAGE_ACTIVATE);

			zend_string_release(chars);
		}

		zend_string_release(local);
	} ZEND_HASH_FOREACH_END();
}

static zend_always_inline void php_parallel_stack_push(HashTable *stack, php_parallel_entry_point_t *entry, php_parallel_monitor_t *monitor, zval *future) {
	php_parallel_stack_el_t el;

	el.entry = *entry;
	el.monitor = monitor;
	el.future = future;

	zend_hash_next_index_insert_mem(stack, &el, sizeof(php_parallel_stack_el_t));
}

#if PHP_VERSION_ID >= 70300
static zend_always_inline php_parallel_stack_el_t* php_parallel_stack_pop(HashTable *stack, HashPosition *position) {
	zval *el = zend_hash_get_current_data_ex(stack, position);

	if (!el) {
		zend_hash_clean(stack);
		zend_hash_internal_pointer_reset_ex(stack, position);
		return NULL;
	}

	if (zend_hash_move_forward_ex(stack, position) != SUCCESS) {
		zend_hash_internal_pointer_reset_ex(stack, position);
	}

	return Z_PTR_P(el);
}
#else
static zend_always_inline php_parallel_stack_el_t* php_parallel_stack_pop(HashTable *stack, HashPosition *position) {
	zval *el = NULL;

	if (*position <= stack->nNumUsed) {
		el = zend_hash_index_find(stack, *position);
	}

	if (!el) {
		zend_hash_clean(stack);
		*position = 0;
		return NULL;
	}

	if (++(*position) > stack->nNumUsed) {
		*position = 0;
	}

	return Z_PTR_P(el);
}
#endif

PHP_METHOD(Parallel, __construct)
{
	php_parallel_t *parallel = php_parallel_from(getThis());
	zend_string    *bootstrap = NULL;
	zval           *configuration = NULL;
	int32_t        state = SUCCESS;

	if (ZEND_NUM_ARGS()) {
		if (zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS(), "a", &configuration) != SUCCESS &&
		    zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS(), "S|a", &bootstrap, &configuration) != SUCCESS) {
			php_parallel_monitor_set(parallel->monitor, PHP_PARALLEL_ERROR, 0);
			php_parallel_exception("bootstrap or bootstrap and optional configuration expected");
			return;
		}
	}

	if (bootstrap) {
		parallel->bootstrap = zend_string_dup(bootstrap, 1);
	}

	if (configuration) {
		ZVAL_COPY(&parallel->configuration, configuration);
	}

	if (pthread_create(&parallel->thread, NULL, php_parallel_routine, parallel) != SUCCESS) {
		php_parallel_exception("cannot create Runtime");
		php_parallel_monitor_set(parallel->monitor, PHP_PARALLEL_ERROR, 0);
		return;
	}

	state = php_parallel_monitor_wait(parallel->monitor, PHP_PARALLEL_READY|PHP_PARALLEL_ERROR);

	if (state & PHP_PARALLEL_ERROR) {
		php_parallel_exception(
			"bootstrapping Runtime failed with %s", ZSTR_VAL(parallel->bootstrap));
		php_parallel_monitor_wait(parallel->monitor, PHP_PARALLEL_DONE);
		php_parallel_monitor_set(parallel->monitor, PHP_PARALLEL_ERROR, 0);
		pthread_join(parallel->thread, NULL);
	}
}

PHP_METHOD(Parallel, run)
{
	php_parallel_t *parallel = php_parallel_from(getThis());
	php_parallel_entry_point_t entry;
	php_parallel_future_t   *future;
	zval *closure = NULL;
	zval *argv = NULL;
	zend_bool returns = 0;

	if (zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS(), "O|a", &closure, zend_ce_closure, &argv) != SUCCESS) {
		php_parallel_exception("Closure, or Closure and args expected");
		return;
	}

	php_parallel_monitor_lock(parallel->monitor);

	if (php_parallel_monitor_check(parallel->monitor, PHP_PARALLEL_CLOSED|PHP_PARALLEL_ERROR)) {
		php_parallel_monitor_unlock(parallel->monitor);
		php_parallel_exception("Runtime unusable");
		return;
	}

	if (!php_parallel_copy_check(&entry, 
		EG(current_execute_data)->prev_execute_data,
		zend_get_closure_method_def(closure),
		ZEND_NUM_ARGS() - 1, argv, &returns)) {
		php_parallel_monitor_unlock(parallel->monitor);
		return;
	}

	if (returns) {
		object_init_ex(return_value, php_parallel_future_ce);

		future = php_parallel_future_from(return_value);
	}

	php_parallel_stack_push(
		&parallel->stack, 
		&entry,
		returns ? future->monitor : NULL, 
		returns ? &future->value : NULL);

	php_parallel_monitor_set(parallel->monitor, PHP_PARALLEL_EXEC, 0);
	php_parallel_monitor_unlock(parallel->monitor);
}

PHP_METHOD(Parallel, close)
{
	php_parallel_t *parallel = 
		php_parallel_from(getThis());

	php_parallel_monitor_lock(parallel->monitor);

	if (php_parallel_monitor_check(parallel->monitor, PHP_PARALLEL_CLOSED|PHP_PARALLEL_ERROR)) {
		php_parallel_exception("Runtime unusable");
		php_parallel_monitor_unlock(parallel->monitor);
		return;
	}

	php_parallel_monitor_set(
		parallel->monitor, PHP_PARALLEL_CLOSE, 0);
	php_parallel_monitor_wait_locked(
		parallel->monitor, PHP_PARALLEL_DONE);

	php_parallel_monitor_unlock(parallel->monitor);

	php_parallel_monitor_set(
		parallel->monitor, PHP_PARALLEL_CLOSED, 0);

	pthread_join(parallel->thread, NULL);
}

PHP_METHOD(Parallel, kill)
{
	php_parallel_t *parallel = 
		php_parallel_from(getThis());

	php_parallel_monitor_lock(parallel->monitor);

	if (php_parallel_monitor_check(parallel->monitor, PHP_PARALLEL_CLOSED|PHP_PARALLEL_ERROR)) {
		php_parallel_exception("Runtime unusable");
		php_parallel_monitor_unlock(parallel->monitor);
		return;
	}

	php_parallel_monitor_set(
		parallel->monitor, PHP_PARALLEL_KILLED, 0);

	*(parallel->child.interrupt) = 1;

	php_parallel_monitor_wait_locked(
		parallel->monitor, PHP_PARALLEL_DONE);

	php_parallel_monitor_unlock(parallel->monitor);

	php_parallel_monitor_set(
		parallel->monitor, PHP_PARALLEL_CLOSED, 0);

	pthread_join(parallel->thread, NULL);
}

zend_function_entry php_parallel_methods[] = {
	PHP_ME(Parallel, __construct, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(Parallel, run, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(Parallel, close, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(Parallel, kill, NULL, ZEND_ACC_PUBLIC)
	PHP_FE_END
};

int php_parallel_stack_kill(zval *zv) {
	php_parallel_stack_el_t *el = Z_PTR_P(zv);

	if (el->monitor) {
		php_parallel_monitor_set(el->monitor, PHP_PARALLEL_KILLED, 1);
	}

	return ZEND_HASH_APPLY_REMOVE;
}

void php_parallel_stack_free(zval *zv) {
	php_parallel_stack_el_t *el = Z_PTR_P(zv);

	php_parallel_copy_free(el->entry.point, 1);

	free(Z_PTR_P(zv));
}

zend_object* php_parallel_create(zend_class_entry *type) {
	php_parallel_t *parallel = ecalloc(1, 
			sizeof(php_parallel_t) + zend_object_properties_size(type));

	zend_object_std_init(&parallel->std, type);

	parallel->std.handlers = &php_parallel_handlers;

	parallel->monitor = php_parallel_monitor_create();

	zend_hash_init(&parallel->stack, 64, NULL, php_parallel_stack_free, 1);
#if PHP_VERSION_ID >= 70300
	zend_hash_internal_pointer_reset_ex(
		&parallel->stack, &parallel->next);
#else
	parallel->next = 0;
#endif

	parallel->parent.server = SG(server_context);

	return &parallel->std;
}

void php_parallel_destroy(zend_object *o) {
	php_parallel_t *parallel = 
		php_parallel_fetch(o);

	php_parallel_monitor_lock(parallel->monitor);

	if (!php_parallel_monitor_check(parallel->monitor, PHP_PARALLEL_CLOSED|PHP_PARALLEL_ERROR)) {
		php_parallel_monitor_set(
			parallel->monitor,
			PHP_PARALLEL_CLOSE, 0);

		php_parallel_monitor_wait_locked(
			parallel->monitor,
			PHP_PARALLEL_DONE);

		php_parallel_monitor_unlock(parallel->monitor);

		pthread_join(parallel->thread, NULL);
	} else {
		php_parallel_monitor_unlock(parallel->monitor);
	}

	php_parallel_monitor_destroy(parallel->monitor);

	if (parallel->bootstrap) {
		zend_string_release(parallel->bootstrap);
	}

	if (!Z_ISUNDEF(parallel->configuration)) {
		zval_ptr_dtor(&parallel->configuration);
	}

	zend_hash_destroy(&parallel->stack);
	
	zend_object_std_dtor(o);
}

void php_parallel_startup(void) {
	zend_class_entry ce;

	memcpy(&php_parallel_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));

	php_parallel_handlers.offset = XtOffsetOf(php_parallel_t, std);
	php_parallel_handlers.free_obj = php_parallel_destroy;

	INIT_NS_CLASS_ENTRY(ce, "parallel", "Runtime", php_parallel_methods);

	php_parallel_ce = zend_register_internal_class(&ce);
	php_parallel_ce->create_object = php_parallel_create;
	php_parallel_ce->ce_flags |= ZEND_ACC_FINAL;

	php_parallel_main = zend_string_init(ZEND_STRL("\\parallel\\Runtime::run"), 1);

	INIT_NS_CLASS_ENTRY(ce, "parallel", "Exception", NULL);

	php_parallel_exception_ce = zend_register_internal_class_ex(&ce, zend_ce_error_exception);

	if (strncmp(sapi_module.name, "cli", sizeof("cli")-1) == SUCCESS) {
		php_sapi_deactivate_function = sapi_module.deactivate;

		sapi_module.deactivate = NULL;
	}

	zend_interrupt_handler = zend_interrupt_function;
	zend_interrupt_function = php_parallel_interrupt;
}

void php_parallel_shutdown(void) {
	if (strncmp(sapi_module.name, "cli", sizeof("cli")-1) == SUCCESS) {
		sapi_module.deactivate = php_sapi_deactivate_function;
	}

	zend_string_release(php_parallel_main);
	
	zend_interrupt_function = zend_interrupt_handler;
}

static zend_always_inline int php_parallel_bootstrap(zend_string *file) {
	zend_file_handle fh;
	zend_op_array *ops;
	zval rv;

	int result = php_stream_open_for_zend_ex(ZSTR_VAL(file), &fh, USE_PATH|REPORT_ERRORS|STREAM_OPEN_FOR_INCLUDE);

	if (result != SUCCESS) {
		return FAILURE;
	}

	if (!fh.opened_path) {
		fh.opened_path = zend_string_dup(file, 0);
	}

	zend_hash_add_empty_element(&EG(included_files), fh.opened_path);

	ops = zend_compile_file(
		&fh, ZEND_REQUIRE);
	zend_destroy_file_handle(&fh);

	if (ops) {
		ZVAL_UNDEF(&rv);
		zend_execute(ops, &rv);
		destroy_op_array(ops);
		efree(ops);

		if (EG(exception)) {
			zend_clear_exception();
			return FAILURE;
		}

		zval_ptr_dtor(&rv);
		return SUCCESS;
	}

	if (EG(exception)) {
		zend_clear_exception();
	}

	return FAILURE;
}

void* php_parallel_routine(void *arg) {	
	int32_t state = 0;

	php_parallel_t *parallel = 
		context = (php_parallel_t*) arg;
	
	ts_resource(0);

	TSRMLS_CACHE_UPDATE();

	SG(server_context) = parallel->parent.server;

	parallel->child.interrupt = &EG(vm_interrupt);

	PG(expose_php)       = 0;
	PG(auto_globals_jit) = 1;

	php_request_startup();

	zend_disable_function(ZEND_STRL("setlocale"));
	zend_disable_function(ZEND_STRL("putenv"));

	PG(during_request_startup)  = 0;
	SG(sapi_started)            = 0;
	SG(headers_sent)            = 1;
	SG(request_info).no_headers = 1;

	if (!Z_ISUNDEF(parallel->configuration)) {
		php_parallel_configure(&parallel->configuration);
	}

	if (parallel->bootstrap && php_parallel_bootstrap(parallel->bootstrap) != SUCCESS) {
		php_parallel_monitor_set(
			parallel->monitor, PHP_PARALLEL_ERROR, 1);

		goto _php_parallel_exit;
	}

	php_parallel_monitor_set(parallel->monitor, PHP_PARALLEL_READY, 1);

	do {
		php_parallel_stack_el_t *el = NULL;

		if (php_parallel_monitor_lock(parallel->monitor) != SUCCESS) {
			break;
		}

		if (php_parallel_monitor_check(parallel->monitor, PHP_PARALLEL_KILLED)) {
_php_parallel_kill:
			zend_hash_apply(
				&parallel->stack, 
				php_parallel_stack_kill);
			php_parallel_monitor_unlock(parallel->monitor);
			goto _php_parallel_exit;
		}

		while (!(el = php_parallel_stack_pop(&parallel->stack, &parallel->next))) {
			if (!(state & PHP_PARALLEL_CLOSE)) {
				state = php_parallel_monitor_wait_locked(
						parallel->monitor,
						PHP_PARALLEL_EXEC|PHP_PARALLEL_CLOSE|PHP_PARALLEL_KILLED);
			}

			if ((state & (PHP_PARALLEL_CLOSE|PHP_PARALLEL_KILLED))) {
				if ((state & PHP_PARALLEL_KILLED)) {
					goto _php_parallel_kill;
				}

				if (!zend_hash_num_elements(&parallel->stack)) {
					php_parallel_monitor_unlock(parallel->monitor);
					goto _php_parallel_exit;
				}
			}
		}

		php_parallel_monitor_unlock(parallel->monitor);

		zend_first_try {
			php_parallel_execute(el->monitor, 
				el->entry.point, &el->entry.argv, el->future);
		} zend_end_try();

		php_parallel_zval_dtor(&el->entry.argv);

		if (el->monitor) {
			php_parallel_monitor_set(el->monitor, PHP_PARALLEL_READY, 1);
		}
	} while (1);

_php_parallel_exit:
	php_parallel_monitor_set(parallel->monitor, PHP_PARALLEL_DONE, 1);

	php_request_shutdown(NULL);

	ts_free_thread();

	pthread_exit(NULL);

	return NULL;
}
#endif
