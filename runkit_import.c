/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2006 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.0 of the PHP license,       |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_0.txt.                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Sara Golemon <pollita@php.net>                               |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#include "php_runkit.h"

#ifdef PHP_RUNKIT_MANIPULATION
/* {{{ php_runkit_import_functions
 */
static int php_runkit_import_functions(HashTable *function_table, long flags TSRMLS_DC)
{
	HashPosition pos;
	zend_function *fe;

	for(zend_hash_internal_pointer_reset_ex(function_table, &pos);
	    zend_hash_get_current_data_ex(function_table, (void**)&fe, &pos) == SUCCESS;
            zend_hash_move_forward_ex(function_table, &pos)) {
		char *key;
		int key_len, type;
		long idx;
		ulong key_hash;
		zend_function fecopy;

		if ((type = zend_hash_get_current_key_ex(function_table, &key, &key_len, &idx, 0, &pos)) != HASH_KEY_IS_STRING) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Ignoring numerically indexed function entry %ld", idx);
			continue;
		}
		key_hash = zend_get_hash_value(key, key_len);

		if (!fe || fe->type != ZEND_USER_FUNCTION) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Ignoring non-user function during import");
			continue;
		}

		if (zend_hash_quick_exists(EG(function_table), key, key_len, key_hash)) {
			if (flags & PHP_RUNKIT_IMPORT_OVERRIDE) {
				php_runkit_function_delete(EG(function_table), key, key_len, key_hash TSRMLS_CC);
			} else {
				/* No override == skip */
				continue;
			}
		}

		/* We shouldn't have to copy this, but... see php_runkit_function_delete() */
		fecopy = *fe;
		php_runkit_function_copy_ctor(&fecopy, NULL);
		if (zend_hash_quick_add(EG(function_table), key, key_len, key_hash, &fecopy, sizeof(zend_function), (void**)&fe) == FAILURE) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failure importing %s()", fecopy.common.function_name);
			php_runkit_function_dtor(&fecopy TSRMLS_CC);
			return FAILURE;
		}
		fe->common.prototype = fe;
	}

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_import_class_methods
 */
static int php_runkit_import_class_methods(zend_class_entry *dce, zend_class_entry *ce, int override TSRMLS_DC)
{
	HashPosition pos;
	zend_function *fe;

	for(zend_hash_internal_pointer_reset_ex(&ce->function_table, &pos);
	    zend_hash_get_current_data_ex(&ce->function_table, (void**)&fe, &pos) == SUCCESS;
	    zend_hash_move_forward_ex(&ce->function_table, &pos)) {
		char *key;
		int key_len;
		long idx;
		ulong key_hash;
		zend_function *dfe, fecopy;
		zend_class_entry *fe_scope = php_runkit_locate_scope(ce, fe, fe->common.function_name, strlen(fe->common.function_name));

		if (zend_hash_get_current_key_ex(&ce->function_table, &key, &key_len, &idx, 0, &pos) != HASH_KEY_IS_STRING) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Ignoring method with non-string name %ld", idx);
			continue;
		}
		key_hash = zend_get_hash_value(key, key_len);

		if (fe_scope != ce) {
			/* This is an inhereted function, let's skip it */
			continue;
		}

		if (zend_hash_quick_find(&dce->function_table, key, key_len, key_hash, (void**)&dfe) == SUCCESS) {
			if (!override) {
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "%s::%s() already exists, not importing",
				                 dce->name, dfe->common.function_name);
				continue;
			}

			zend_class_entry *scope = php_runkit_locate_scope(dce, dfe, key, key_len - 1);

			if (php_runkit_check_call_stack(&dfe->op_array TSRMLS_CC) == FAILURE) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cannot override active method %s::%s(). Skipping.", dce->name, fe->common.function_name);
				continue;
			}

			zend_hash_apply_with_arguments(EG(class_table) ZEND_HASH_APPLY_ARGS_TSRMLS_CC, (apply_func_args_t)php_runkit_clean_children_methods, 4, scope, dce, key, key_len - 1);
			if (php_runkit_function_delete(&dce->function_table, key, key_len, key_hash TSRMLS_CC) == FAILURE) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Error removing old method in destination class %s::%s", dce->name, fe->common.function_name);
				continue;
			}
		}

		fecopy = *fe;
		php_runkit_function_copy_ctor(&fecopy, NULL);
#ifdef ZEND_ENGINE_2
		fecopy.common.scope = dce;
#endif
		zend_hash_apply_with_arguments(EG(class_table) ZEND_HASH_APPLY_ARGS_TSRMLS_CC, (apply_func_args_t)php_runkit_update_children_methods, 5, dce, dce, &fecopy, key, key_len - 1);

		if (zend_hash_quick_add(&dce->function_table, key, key_len, key_hash, &fecopy, sizeof(zend_function), (void**)&fe) == FAILURE) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failure importing %s::%s()", ce->name, fe->common.function_name);
			php_runkit_function_dtor(&fecopy TSRMLS_CC);
			continue;
		}
		fe->common.prototype = fe;
	}

	return SUCCESS;
}
/* }}} */

#ifdef ZEND_ENGINE_2
/* {{{ php_runkit_import_class_consts
 */
static int php_runkit_import_class_consts(zend_class_entry *dce, zend_class_entry *ce, int override TSRMLS_DC)
{
	HashPosition pos;
	char *key;
	int key_len;
	long idx;
	zval **c;

	zend_hash_internal_pointer_reset_ex(&ce->constants_table, &pos);
	while (zend_hash_get_current_data_ex(&ce->constants_table, (void**)&c, &pos) == SUCCESS && c && *c) {
		long action = HASH_ADD;

		if (zend_hash_get_current_key_ex(&ce->constants_table, &key, &key_len, &idx, 0, &pos) == HASH_KEY_IS_STRING) {
			if (zend_hash_exists(&dce->constants_table, key, key_len)) {
				if (override) {
					action = HASH_UPDATE;
				} else {
					php_error_docref(NULL TSRMLS_CC, E_NOTICE, "%s::%s already exists, not importing", dce->name, key);
					goto import_const_skip;
				}
			}
			Z_ADDREF_P(*c);
			if (zend_hash_add_or_update(&dce->constants_table, key, key_len, (void*)c, sizeof(zval*), NULL, action) == FAILURE) {
				zval_ptr_dtor(c);
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to import %s::%s", dce->name, key);
			}

			zend_hash_apply_with_arguments(EG(class_table) ZEND_HASH_APPLY_ARGS_TSRMLS_CC, (apply_func_args_t)php_runkit_update_children_consts, 4, dce, c, key, key_len - 1);
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Constant has invalid key name");
		}
import_const_skip:
		zend_hash_move_forward_ex(&ce->constants_table, &pos);
	}
	return SUCCESS;
}
/* }}} */
#endif

/* {{{ php_runkit_import_class_props
 */
static int php_runkit_import_class_props(zend_class_entry *dce, zend_class_entry *ce, int override TSRMLS_DC)
{
	HashPosition pos;

#ifdef ZEND_ENGINE_2_4
	zend_property_info *pi;

	for(zend_hash_internal_pointer_reset_ex(&ce->properties_info, &pos);
	    zend_hash_get_current_data_ex(&ce->properties_info, (void**)&pi, &pos) == SUCCESS && pi;
	    zend_hash_move_forward_ex(&ce->properties_info, &pos)) {
		zval ***default_table_ptr = (pi->flags & ZEND_ACC_STATIC) ?
		                            &(dce->default_static_members_table) :
		                            &(dce->default_properties_table);
		int *default_count_ptr = (pi->flags & ZEND_ACC_STATIC) ?
                                         &(dce->default_static_members_count) :
                                         &(dce->default_properties_count);
		zval **source_table = (pi->flags & ZEND_ACC_STATIC) ?
		                      ce->default_static_members_table :
		                      ce->default_properties_table;
		char *key;
		int key_len;
		long idx;
		zend_property_info *dpi;

		if (HASH_KEY_IS_STRING != zend_hash_get_current_key_ex(&ce->properties_info, &key, &key_len, &idx, 0, &pos)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Property has invalid key name");
			continue;
		}

		if (FAILURE == zend_hash_find(&dce->properties_info, key, key_len, (void**)&dpi)) {
			zend_property_info newpi = *pi;
			newpi.name = estrndup(newpi.name, newpi.name_length);
			if (newpi.doc_comment) {
				newpi.doc_comment = estrndup(newpi.doc_comment, newpi.doc_comment_len);
			}
			*default_table_ptr = safe_erealloc(*default_table_ptr, *default_count_ptr + 1, sizeof(zval*), 0);
			newpi.offset = (*default_count_ptr)++;
			MAKE_STD_ZVAL((*default_table_ptr)[newpi.offset]);

			zend_hash_add(&dce->properties_info, key, key_len, &newpi, sizeof(zend_property_info), (void**)&dpi);
		} else if (!override) {
			php_error_docref(NULL TSRMLS_CC, E_NOTICE, "%s::%s already exists, not importing",
			                 dce->name, pi->name);
			continue;
		} else if ((pi->flags & ZEND_ACC_STATIC) != (dpi->flags & ZEND_ACC_STATIC)) {
			/* Awkward case, static to non-static (or vice-versa) */
			zval **oldprop_table = (dpi->flags & ZEND_ACC_STATIC) ?
			                       dce->default_static_members_table :
			                       dce->default_properties_table;
			zval *v = oldprop_table[dpi->offset];
			/* TODO: Shuffle last prop into this offset, for now use a dummy val */
			oldprop_table[dpi->offset] = EG(uninitialized_zval_ptr);
			*default_table_ptr = safe_erealloc(*default_table_ptr, *default_count_ptr + 1, sizeof(zval*), 0);
			dpi->offset = (*default_count_ptr)++;
			(*default_table_ptr)[dpi->offset] = v;
		}
		dpi->flags = pi->flags;
		dpi->ce = dce;
		zval_ptr_dtor(&((*default_table_ptr)[dpi->offset]));
		MAKE_STD_ZVAL((*default_table_ptr)[dpi->offset]);
		ZVAL_ZVAL((*default_table_ptr)[dpi->offset], source_table[pi->offset], 1, 0);

		if (pi->flags & (ZEND_ACC_PUBLIC|ZEND_ACC_PROTECTED)) {
			zend_hash_apply_with_arguments(EG(class_table) ZEND_HASH_APPLY_ARGS_TSRMLS_CC, (apply_func_args_t)php_runkit_update_children_def_props, 5, dce, (*default_table_ptr)[dpi->offset], key, key_len - 1, dpi);
		}
	}

#else /* PHP <= 5.3 */
	zval **p;

	/* Ignores properties_info because that can just be resolved on the fly in these versions */
	for(zend_hash_internal_pointer_reset_ex(&ce->default_properties, &pos);
	    zend_hash_get_current_data_ex(&ce->default_properties, (void**)&p, &pos) == SUCCESS && p && *p;
	    zend_hash_move_forward_ex(&ce->default_properties, &pos)) {
		char *key, *cname = NULL, *pname;
		int key_len;
		long idx;
		long action = HASH_ADD;

		if (HASH_KEY_IS_STRING != zend_hash_get_current_key_ex(&ce->default_properties, &key, &key_len, &idx, 0, &pos)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Property has invalid key name");
			continue;
		}
		pname = key;

#ifdef ZEND_ENGINE_2_2
		zend_unmangle_property_name(key, key_len - 1, &cname, &pname);
#elif defined(ZEND_ENGINE_2)
		zend_unmangle_property_name(key, &cname, &pname);
#endif
		if (zend_hash_exists(&dce->default_properties, key, key_len)) {
			if (override) {
				action = HASH_UPDATE;
			} else {
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "%s->%s already exists, not importing", dce->name, pname);
				continue;
			}
		}
		Z_ADDREF_P(*p);
		if (zend_hash_add_or_update(&dce->default_properties, key, key_len, (void*)p, sizeof(zval*), NULL, action) == FAILURE) {
			zval_ptr_dtor(p);
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to import %s->%s", dce->name, pname);
			continue;
		}

		if (!cname || strcmp(cname, "*") == 0) {
			/* PUBLIC || PROTECTED */
			zend_hash_apply_with_arguments(EG(class_table) ZEND_HASH_APPLY_ARGS_TSRMLS_CC, (apply_func_args_t)php_runkit_update_children_def_props, 4, dce, p, key, key_len - 1);
		}
	}
#endif /* ZEND_ENGINE_2_4 */

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_import_classes
 */
static int php_runkit_import_classes(HashTable *class_table, long flags TSRMLS_DC)
{
	HashPosition pos;
	int i, class_count;

	class_count = zend_hash_num_elements(class_table);
	zend_hash_internal_pointer_reset_ex(class_table, &pos);
	for(i = 0; i < class_count; i++) {
		zend_class_entry *ce = NULL;
		char *key;
		int key_len, type;
		long idx;

		zend_hash_get_current_data_ex(class_table, (void**)&ce, &pos);
#ifdef ZEND_ENGINE_2
		if (ce) {
			ce = *((zend_class_entry**)ce);
		}
#endif
		if (!ce) {
			php_error_docref(NULL TSRMLS_CC, E_ERROR, "Non-class in class table!");
			return FAILURE;
		}

		if (((type = zend_hash_get_current_key_ex(class_table, &key, &key_len, &idx, 0, &pos)) != HASH_KEY_NON_EXISTANT) && 
			ce && ce->type == ZEND_USER_CLASS) {
			zend_class_entry *dce;

			if (!zend_hash_exists(EG(class_table), key, key_len)) {
				/* "Importing" new class, just add it as a reference and move on */
#ifdef ZEND_ENGINE_2
				ce->refcount++;
#else
				php_error_docref(NULL TSRMSL_CC, E_ERROR, "Apathy error: Making the \"import\" of new class definitions in PHP4 is just more hacky than it's worth.  Solve this by having `class %s {}` in your codebase before calling runkit_import() or upgrade to PHP5 and join us in the 21st century.  We'll leave the download server running for you.", ce->name);
#endif
				zend_hash_add(EG(class_table), key, key_len, (void**)&ce, sizeof(zend_class_entry*), NULL);
				return SUCCESS;
			}

			if (php_runkit_fetch_class(key, key_len - 1, &dce TSRMLS_CC) == FAILURE) {
				/* Oddly non-existant target class or error retreiving it... Or it's an internal class... */
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cannot redeclare class %s", ce->name);
				continue;
			}
			
			if (flags & PHP_RUNKIT_IMPORT_CLASS_METHODS) {
				php_runkit_import_class_methods(dce, ce, (flags & PHP_RUNKIT_IMPORT_OVERRIDE) TSRMLS_CC);
			}

#ifdef ZEND_ENGINE_2
			if (flags & PHP_RUNKIT_IMPORT_CLASS_CONSTS) {
				php_runkit_import_class_consts(dce, ce, (flags & PHP_RUNKIT_IMPORT_OVERRIDE) TSRMLS_CC);
			}
#endif

			if (flags & PHP_RUNKIT_IMPORT_CLASS_PROPS) {
				php_runkit_import_class_props(dce, ce, (flags & PHP_RUNKIT_IMPORT_OVERRIDE) TSRMLS_CC);
			}

			zend_hash_move_forward_ex(class_table, &pos);

			if (type == HASH_KEY_IS_STRING) {
				if (zend_hash_del(class_table, key, key_len) == FAILURE) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to remove temporary version of class %s", ce->name);
					continue;
				}
			} else {
				if (zend_hash_index_del(class_table, idx) == FAILURE) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to remove temporary version of class %s", ce->name);
					continue;
				}
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Can not find class definition in class table");
			return FAILURE;
		}
	}

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_compile_filename
 * Duplicate of Zend's compile_filename which explicitly calls the internal compile_file() implementation.
 *
 * This is only used when an accelerator has replaced zend_compile_file() with an alternate method
 * which has been known to cause issues with overly-optimistic early binding.
 *
 * It would be clener to temporarily set zend_compile_file back to compile_file, but that wouldn't be
 * particularly thread-safe so.... */
static zend_op_array *php_runkit_compile_filename(int type, zval *filename TSRMLS_DC)
{
	zend_file_handle file_handle;
	zval tmp;
	zend_op_array *retval;
	char *opened_path = NULL;

	if (filename->type != IS_STRING) {
		tmp = *filename;
		zval_copy_ctor(&tmp);
		convert_to_string(&tmp);
		filename = &tmp;
	}
	file_handle.filename = filename->value.str.val;
	file_handle.free_filename = 0;
	file_handle.type = ZEND_HANDLE_FILENAME;
	file_handle.opened_path = NULL;
#if PHP_MAJOR_VERSION > 5 || (PHP_MINOR_VERSION == 5 && PHP_MINOR_VERSION > 0)
	file_handle.handle.fp = NULL;
#endif

	/* Use builtin compiler only -- bypass accelerators and whatnot */
	retval = compile_file(&file_handle, type TSRMLS_CC);
#ifdef ZEND_ENGINE_2
	if (retval && file_handle.handle.stream.handle) {
#else /* ZEND ENGINE 1 */
	if (retval && ZEND_IS_VALID_FILE_HANDLE(&file_handle)) {
#endif
		int dummy = 1;

		if (!file_handle.opened_path) {
			file_handle.opened_path = opened_path = estrndup(filename->value.str.val, filename->value.str.len);
		}

		zend_hash_add(&EG(included_files), file_handle.opened_path, strlen(file_handle.opened_path)+1, (void *)&dummy, sizeof(int), NULL);

		if (opened_path) {
			efree(opened_path);
		}
	}
	zend_destroy_file_handle(&file_handle TSRMLS_CC);

	if (filename==&tmp) {
		zval_dtor(&tmp);
	}
	return retval;
}
/* }}} */

/* {{{ array runkit_import(string filename[, long flags])
	Import functions and class definitions from a file 
	Similar to include(), but doesn't execute root op_array, and allows pre-existing functions/methods to be overridden */
PHP_FUNCTION(runkit_import)
{
	zend_op_array *new_op_array;
	zval *filename;
	long flags = PHP_RUNKIT_IMPORT_CLASS_METHODS;
	HashTable *current_class_table, *class_table, *current_function_table, *function_table;

	zend_op_array *(*local_compile_filename)(int type, zval *filename TSRMLS_DC) = compile_filename;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|l", &filename, &flags) == FAILURE) {
		RETURN_FALSE;
	}
	convert_to_string(filename);

	if (compile_file != zend_compile_file) {
		/* An accellerator or other dark force is at work
		 * Use the wrapper method to force the builtin compiler
		 * to be used */
		local_compile_filename = php_runkit_compile_filename;
	}

	class_table = (HashTable *) emalloc(sizeof(HashTable));
	zend_hash_init_ex(class_table, 10, NULL, ZEND_CLASS_DTOR, 0, 0);
	function_table = (HashTable *) emalloc(sizeof(HashTable));
	zend_hash_init_ex(function_table, 100, NULL, ZEND_FUNCTION_DTOR, 0, 0);	

	current_class_table = CG(class_table);
	CG(class_table) = class_table;
	current_function_table = CG(function_table);
	CG(function_table) = function_table;

	zend_try {
		new_op_array = local_compile_filename(ZEND_INCLUDE, filename TSRMLS_CC);
	} zend_catch {
		CG(class_table) = current_class_table;
		CG(function_table) = current_function_table;
		zend_hash_destroy(class_table);
		efree(class_table);
		zend_hash_destroy(function_table);
		efree(function_table);
		zend_bailout();
	} zend_end_try();

	CG(class_table) = current_class_table;
	CG(function_table) = current_function_table;
	
	if (!new_op_array) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Import Failure");
		zend_hash_destroy(class_table);
		efree(class_table);
		zend_hash_destroy(function_table);
		efree(function_table);
		RETURN_FALSE;
	}

	/* We never really needed the main loop opcodes to begin with */
	php_runkit_function_dtor((zend_function*)new_op_array TSRMLS_CC);
	efree(new_op_array);

	if (flags & PHP_RUNKIT_IMPORT_FUNCTIONS) {
		php_runkit_import_functions(function_table, flags TSRMLS_CC);
	}

	if (flags & PHP_RUNKIT_IMPORT_CLASSES) {
		php_runkit_import_classes(class_table, flags TSRMLS_CC);
	}

	zend_hash_destroy(class_table);
	efree(class_table);
	zend_hash_destroy(function_table);
	efree(function_table);

	RETURN_TRUE;
}
/* }}} */
#endif /* PHP_RUNKIT_MANIPULATION */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
