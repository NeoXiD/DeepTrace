/*
 * +------------------------------------------------------------------------+
 * | DeepTrace (Website: http://www.snapserv.net/)							|
 * +------------------------------------------------------------------------+
 * | Copyright (c) 2012-2013 	P. Mathis (pmathis@snapserv.net)			|
 * |							Y. Khalil (dev@pp3345.net)					|
 * +------------------------------------------------------------------------+
 * | Licensed under the Apache License, Version 2.0 (the "License");		|
 * | you may not use this file except in compliance with the License.		|
 * | You may obtain a copy of the License at								|
 * |																		|
 * |	http://www.apache.org/licenses/LICENSE-2.0							|
 * |																		|
 * | Unless required by applicable law or agreed to in writing, software	|
 * | distributed under the License is distributed on an "AS IS" BASIS,		|
 * | WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or		|
 * | implied. See the License for the specific language governing 			|
 * | permissions and limitations under the License.							|
 * +------------------------------------------------------------------------+
 */

#include "php_DeepTrace.h"

/* {{{ DeepTrace_fetch_function */
static int DeepTrace_fetch_function(char *funcName, int funcName_len, zend_function **funcPtr,
		int flag, ulong hash TSRMLS_DC)
{
	zend_function *func;

	/* Find function in hash table */
	if(UNEXPECTED(zend_hash_quick_find(EG(function_table), funcName, funcName_len + 1, hash, (void**) &func) == FAILURE)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Function %s() does not exist.", funcName);
		return FAILURE;
	}

	/* Check if function is a special function which should not be touched */
	if(UNEXPECTED(func->type != ZEND_USER_FUNCTION && func->type != ZEND_INTERNAL_FUNCTION)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s() is not a user / internal function.", funcName);
		return FAILURE;
	}

	if(funcPtr) *funcPtr = func;

	/* Store modifications for internal functions */
	if(func->type == ZEND_INTERNAL_FUNCTION && flag >= DEEPTRACE_FUNCTION_REMOVE) {
		/* Allocate hash table if necessary */
		if(UNEXPECTED(!DEEPTRACE_G(replaced_internal_functions))) {
			ALLOC_HASHTABLE(DEEPTRACE_G(replaced_internal_functions));
			zend_hash_init(DEEPTRACE_G(replaced_internal_functions), 4, NULL, NULL, 0);
		}
		zend_hash_quick_add(DEEPTRACE_G(replaced_internal_functions), funcName, funcName_len + 1,
				hash, (void *) func, sizeof(zend_function), NULL);

		if(flag >= DEEPTRACE_FUNCTION_RENAME) {
			zend_hash_key hashKey;

			/* Allocate hash table if necessary */
			if(UNEXPECTED(!DEEPTRACE_G(misplaced_internal_functions))) {
				ALLOC_HASHTABLE(DEEPTRACE_G(misplaced_internal_functions));
				zend_hash_init(DEEPTRACE_G(misplaced_internal_functions), 4, NULL, NULL, 0);
			}
			hashKey.nKeyLength = funcName_len + 1;
			hashKey.arKey = estrndup(funcName, funcName_len);
			zend_hash_next_index_insert(DEEPTRACE_G(misplaced_internal_functions),
					(void *) &hashKey, sizeof(zend_hash_key), NULL);
		}
	}

	return SUCCESS;
}
/* }}} */

/* {{{ DeepTrace_destroy_misplaced_functions */
static int DeepTrace_destroy_misplaced_functions(zend_hash_key *hashKey TSRMLS_DC)
{
	if(!hashKey->nKeyLength) return ZEND_HASH_APPLY_REMOVE;
	zend_hash_del(EG(function_table), hashKey->arKey, hashKey->nKeyLength);
	efree((void*) hashKey->arKey);
	return ZEND_HASH_APPLY_REMOVE;
}
/* }}} */

/* {{{ DeepTrace_restore_internal_functions */
static int DeepTrace_restore_internal_functions(zend_internal_function *func TSRMLS_DC, int numArgs,
		va_list args, zend_hash_key *hashKey)
{
	if(!hashKey->nKeyLength) return ZEND_HASH_APPLY_REMOVE;

	zend_hash_update(EG(function_table), hashKey->arKey, hashKey->nKeyLength,
			(void *) func, sizeof(zend_function), NULL);
	return ZEND_HASH_APPLY_REMOVE;
}
/* }}} */

/* {{{ DeepTrace_delete_user_functions */
static int DeepTrace_delete_user_functions(void *dest TSRMLS_DC)
{
	if(dest == NULL || ((zend_function *) dest)->type != ZEND_INTERNAL_FUNCTION) return ZEND_HASH_APPLY_REMOVE;
	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

/* {{{ DeepTrace_functions_cleanup */
void DeepTrace_functions_cleanup(TSRMLS_D)
{
	if(DEEPTRACE_G(misplaced_internal_functions)) {
		zend_hash_apply(DEEPTRACE_G(misplaced_internal_functions),
				(apply_func_t) DeepTrace_destroy_misplaced_functions TSRMLS_CC);
		zend_hash_destroy(DEEPTRACE_G(misplaced_internal_functions));
		FREE_HASHTABLE(DEEPTRACE_G(misplaced_internal_functions));
	}

	if(DEEPTRACE_G(replaced_internal_functions)) {
		zend_hash_apply_with_arguments(DEEPTRACE_G(replaced_internal_functions) TSRMLS_CC,
				(apply_func_args_t) DeepTrace_restore_internal_functions, 1);
		zend_hash_destroy(DEEPTRACE_G(replaced_internal_functions));
		FREE_HASHTABLE(DEEPTRACE_G(replaced_internal_functions));
	}

	zend_hash_apply(EG(function_table), DeepTrace_delete_user_functions TSRMLS_CC);

	DEEPTRACE_G(replaced_internal_functions) = NULL;
	DEEPTRACE_G(misplaced_internal_functions) = NULL;
}
/* }}} */

/* {{{ DeepTrace_clear_function_runtime_cache */
static int DeepTrace_clear_function_runtime_cache(void *pDest TSRMLS_DC)
{
	zend_function *f = (zend_function *) pDest;

	if(pDest == NULL || f->type != ZEND_USER_FUNCTION ||
			f->op_array.last_cache_slot == 0 || f->op_array.run_time_cache == NULL) {
		return ZEND_HASH_APPLY_KEEP;
	}

	memset(f->op_array.run_time_cache, 0, (f->op_array.last_cache_slot) * sizeof(void *));
	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

/* {{{ DeepTrace_clear_all_functions_runtime_cache */
void DeepTrace_clear_all_functions_runtime_cache(TSRMLS_D)
{
	int i, count;
	zend_execute_data *ptr;
	HashPosition hashPos;

	zend_hash_apply(EG(function_table), (apply_func_t) DeepTrace_clear_function_runtime_cache TSRMLS_CC);
	zend_hash_internal_pointer_reset_ex(EG(class_table), &hashPos);
	count = zend_hash_num_elements(EG(class_table));

	for(i = 0; i < count; ++i) {
		zend_class_entry **ce;
		zend_hash_get_current_data_ex(EG(class_table), (void *) &ce, &hashPos);
		zend_hash_apply(&(* ce)->function_table, (apply_func_t) DeepTrace_clear_function_runtime_cache TSRMLS_CC);
		zend_hash_move_forward_ex(EG(class_table), &hashPos);
	}

	for(ptr = EG(current_execute_data); ptr != NULL; ptr = ptr->prev_execute_data) {
		if(ptr->op_array == NULL || ptr->op_array->last_cache_slot == 0 || ptr->op_array->run_time_cache == NULL) {
			continue;
		}
		memset(ptr->op_array->run_time_cache, 0, (ptr->op_array->last_cache_slot) * sizeof(void *));
	}

	if(!EG(objects_store).object_buckets) {
		return;
	}

	for(i = 1; i < EG(objects_store).top; i++) {
		if(EG(objects_store).object_buckets[i].valid && (!EG(objects_store).object_buckets[i].destructor_called) &&
				EG(objects_store).object_buckets[i].bucket.obj.object) {
			zend_object *object;
			object = (zend_object *) EG(objects_store).object_buckets[i].bucket.obj.object;
			if(object->ce == zend_ce_closure) {
				zend_closure *cl = (zend_closure *) object;
				DeepTrace_clear_function_runtime_cache((void *) &cl->func TSRMLS_CC);
			}
		}
	}
}
/* }}} */

/* {{{ proto bool dt_rename_function(string oldName, string newName)
   Renames a function */
PHP_FUNCTION(dt_rename_function)
{
	DEEPTRACE_DECL_STRING_PARAM(oldFuncName);
	DEEPTRACE_DECL_STRING_PARAM(newFuncName);
	zend_function *oldFunc, newFunc;
	ulong oldFuncHash, newFuncHash;

	if(UNEXPECTED(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss",
			DEEPTRACE_STRING_PARAM(oldFuncName),
			DEEPTRACE_STRING_PARAM(newFuncName)) == FAILURE)) {
		RETURN_FALSE;
	}

	/* Convert function names to lowercase and generate hashes */
	oldFuncName = zend_str_tolower_dup(oldFuncName, oldFuncName_len);
	newFuncName = zend_str_tolower_dup(newFuncName, newFuncName_len);
	oldFuncHash = zend_inline_hash_func(oldFuncName, oldFuncName_len + 1);
	newFuncHash = zend_inline_hash_func(newFuncName, newFuncName_len + 1);

	/* Check if new function name is free / not already in use */
	if(UNEXPECTED(zend_hash_quick_exists(EG(function_table), newFuncName, newFuncName_len + 1, newFuncHash))) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "New function name %s() is already in use.", newFuncName);
		efree(oldFuncName);
		efree(newFuncName);
		RETURN_FALSE;
	}

	/* Check if old function exists */
	if(UNEXPECTED(DeepTrace_fetch_function(oldFuncName, oldFuncName_len, &oldFunc,
			DEEPTRACE_FUNCTION_RENAME, oldFuncHash TSRMLS_CC) == FAILURE)) {
		efree(oldFuncName);
		efree(newFuncName);
		RETURN_FALSE;
	}

	/* Create new function by reference */
	newFunc = (zend_function) *oldFunc;
	function_add_ref(&newFunc);

	/* Remove old function reference from hashtable */
	if(UNEXPECTED(zend_hash_quick_del(EG(function_table), oldFuncName, oldFuncName_len + 1, oldFuncHash) == FAILURE)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Can not remove old reference to function %s().", oldFuncName);
		efree(oldFuncName);
		efree(newFuncName);
		RETURN_FALSE;
	}

	/* Change function name if it is an user function */
	if(newFunc.type == ZEND_USER_FUNCTION) {
		efree((void *) newFunc.common.function_name);
		newFunc.common.function_name = estrndup(newFuncName, newFuncName_len);
	}

	/* Add new function reference */
	if(UNEXPECTED(zend_hash_quick_add(EG(function_table), newFuncName, newFuncName_len + 1,
			newFuncHash, &newFunc, sizeof(zend_function), NULL) == FAILURE)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Can not create new reference to function %s().", newFuncName);
		zend_function_dtor(&newFunc);
		efree(oldFuncName);
		efree(newFuncName);
		RETURN_FALSE;
	}

	DeepTrace_clear_all_functions_runtime_cache(TSRMLS_C);
	efree(oldFuncName);
	efree(newFuncName);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool dt_remove_function(string functionName)
   Removes a function */
PHP_FUNCTION(dt_remove_function)
{
	DEEPTRACE_DECL_STRING_PARAM(functionName);
	ulong hash;

	if(UNEXPECTED(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s",
			DEEPTRACE_STRING_PARAM(functionName)) == FAILURE)) {
		RETURN_FALSE;
	}

	/* Convert function name to lowercase and generate hash */
	functionName = zend_str_tolower_dup(functionName, functionName_len);
	hash = zend_inline_hash_func(functionName, functionName_len + 1);

	if(UNEXPECTED(!zend_hash_quick_exists(EG(function_table), functionName, functionName_len + 1, hash))) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Function %s does not exist.", functionName);
		efree(functionName);
		RETURN_FALSE;
	}

	/* Kill the autoloader if the function name equals to '__autoload' */
	if(functionName_len == sizeof("__autoload") - 1 && !strcmp(functionName, "__autoload")) {
		EG(autoload_func) = NULL;
	}

	/* DeepTrace internal stuff (so functions can be restored when a new request begins) */
	if(UNEXPECTED(DeepTrace_fetch_function(functionName, functionName_len, NULL,
			DEEPTRACE_FUNCTION_REMOVE, hash TSRMLS_CC) == FAILURE)) {
		efree(functionName);
		RETURN_FALSE;
	}

	/* Delete the function */
	zend_hash_quick_del(EG(function_table), functionName, functionName_len + 1, hash);
	DeepTrace_clear_all_functions_runtime_cache(TSRMLS_C);
	efree(functionName);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool dt_set_static_function_variable(string functionName, string variableName, mixed value)
   Sets the value of a static variable inside a function */
PHP_FUNCTION(dt_set_static_function_variable)
{
	DEEPTRACE_DECL_STRING_PARAM(functionName);
	DEEPTRACE_DECL_STRING_PARAM(variableName);
	int refcount;
	zval *value, **variablePointer;
	ulong functionHash, variableHash;
	zend_function *func;
	zend_uchar isRef;

	if(UNEXPECTED(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ssz",
			DEEPTRACE_STRING_PARAM(functionName),
			DEEPTRACE_STRING_PARAM(variableName),
			&value) == FAILURE)) {
		RETURN_FALSE;
	}

	/* Make function name lowercase and get hashs */
	functionName = zend_str_tolower_dup(functionName, functionName_len);
	functionHash = zend_inline_hash_func(functionName, functionName_len + 1);
	variableHash = zend_inline_hash_func(variableName, variableName_len + 1);

	/* Fetch function */
	if(UNEXPECTED(DeepTrace_fetch_function(functionName, functionName_len, &func,
			DEEPTRACE_FUNCTION_SET_STATIC_VAR, functionHash TSRMLS_CC) == FAILURE)) {
		efree(functionName);
		RETURN_FALSE;
	}

	/* Get pointer to static variable */
	if(UNEXPECTED(func->op_array.static_variables == NULL || zend_hash_quick_find(func->op_array.static_variables,
			variableName, variableName_len + 1, variableHash, (void **) &variablePointer) == FAILURE)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Static variable %s does not exist.", variableName);
		efree(functionName);
		RETURN_FALSE;
	}

	/* Change value of static variable */
	refcount = Z_REFCOUNT_PP(variablePointer);
	isRef = Z_ISREF_PP(variablePointer);
	zval_dtor(*variablePointer);
	**variablePointer = *value;
	zval_copy_ctor(*variablePointer);
	Z_SET_REFCOUNT_PP(variablePointer, refcount);
	Z_SET_ISREF_TO_PP(variablePointer, isRef);

	efree(functionName);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool dt_destroy_function_data(string functionName)
   Destroys the data of a function */
PHP_FUNCTION(dt_destroy_function_data)
{
	DEEPTRACE_DECL_STRING_PARAM(functionName);
	zend_function *func;
	ulong hash;

	if(UNEXPECTED(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s",
			DEEPTRACE_STRING_PARAM(functionName)) == FAILURE)) {
		RETURN_FALSE;
	}

	/* Convert function name to lowercase and get hash */
	functionName = zend_str_tolower_dup(functionName, functionName_len);
	hash = zend_inline_hash_func(functionName, functionName_len + 1);
	if(UNEXPECTED(DeepTrace_fetch_function(functionName, functionName_len, &func, 0, hash TSRMLS_CC) == FAILURE)) {
		efree(functionName);
		RETURN_FALSE;
	}

	/* Cleanup function data */
	zend_cleanup_function_data_full(func TSRMLS_CC);
	efree(functionName);
	RETURN_TRUE;
}
/* }}} */
