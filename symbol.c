/*
 * symbol.c - Implementation of symbol data.
 *
 * Copyright (C) 2014 Andrew Schwartzmeyer
 *
 * This file released under the AGPLv3 license.
 */
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "symbol.h"
#include "args.h"
#include "logger.h"

#include "type.h"
#include "node.h"
#include "token.h"
#include "libs.h"
#include "rules.h"
#include "scope.h"

#include "lexer.h"
#include "parser.tab.h"

#include "list.h"
#include "hasht.h"
#include "tree.h"

#define child(i) tree_index(n, i)

/* type comparators */
extern struct typeinfo int_type;
extern struct typeinfo float_type;
extern struct typeinfo char_type;
extern struct typeinfo string_type;
extern struct typeinfo bool_type;
extern struct typeinfo void_type;
extern struct typeinfo class_type;
extern struct typeinfo unknown_type;
extern struct typeinfo ptr_type;

/* local functions */
static void symbol_insert(char *k, struct typeinfo *v, struct tree *t,
                          struct hasht *l, bool constant);

static struct token *get_category(struct tree *n, int target, int before);
static struct token *get_category_(struct tree *n, int target, int before);
static bool get_public(struct tree *n);
static bool get_private(struct tree *n);

static struct typeinfo *get_left_type(struct tree *n);
static struct typeinfo *get_right_type(struct tree *n);

static bool handle_node(struct tree *n, int d);
static void handle_init(struct typeinfo *v, struct tree *n);
static void handle_init_list(struct typeinfo *v, struct tree *n);
static void handle_function(struct typeinfo *t, struct tree *n, char *k);
static void handle_class(struct typeinfo *t, struct tree *n);
static void handle_param(struct typeinfo *v, struct tree *n, struct hasht *s, struct list *l);
void handle_param_list(struct tree *n, struct hasht *s, struct list *l);

/*
 * Populate a global symbol table given the root of a syntax tree.
 */
void symbol_populate(struct tree *t)
{
	set_type_comparators();

	/* do a top-down pre-order traversal to populate symbol tables */
	tree_traverse(t, 0, &handle_node, NULL, NULL);
}

/*
 * Insert symbol as ident/typeinfo pair.
 *
 * Tree used to find token on error. If attempting to insert a
 * function symbol, if the function has been previously declared, it
 * will define the function with the given symbol table. Will error
 * for duplicate symbols or mismatched function declarations.
 */
static void symbol_insert(char *k, struct typeinfo *v, struct tree *t,
                          struct hasht *l, bool constant)
{
	if (v == NULL)
		log_error("symbol_insert(): type for %s was null", k);

	struct typeinfo *e = NULL;
	if (l && v->base == FUNCTION_T)
		/* search for function declaration to define */
		e = scope_search(k);
	else
		/* search just current scope */
		e = hasht_search(list_tail(yyscopes)->data, k);

	if (e == NULL) {
		/* assign region and offset to node AND symbol */
		struct node *n = t->data;
		v->place.region = n->place.region = region;
		v->place.offset = n->place.offset = offset;
		v->place.type = n->place.type = v;

		hasht_insert(constant ? scope_constant() : scope_current(), k, v);
		log_symbol(k, v);

		/* increment offset if not a constant int, bool, or char */
		if (constant) {
			/* floats get 8 bytes in .string */
			if (v->base == FLOAT_T)
				offset += 8;
			/* strings get ssize */
			else if (v->base == CHAR_T && v->pointer)
				offset += v->token->ssize;
			/* ints, chars, and bools use ival directly */
			else
				v->place.offset = n->place.offset = v->token->ival;
		} else {
			offset += typeinfo_size(v);
		}

	} else if (e->base == FUNCTION_T && v->base == FUNCTION_T) {
		if (!typeinfo_compare(e, v)) {
			log_semantic(t, "function signatures for %s mismatched", k);
		} else if (l) {
			/* define the function */
			if (e->function.symbols == NULL) {
				e->function.symbols = l;
				/* fixup param size since declarations forget it */
				e->function.param_size = v->function.param_size;
				log_check("function %s defined", k);
			} else {
				log_semantic(t, "function %s already defined", k);
			}
		}
	} else {
		log_semantic(t, "identifier %s already declared", k);
	}
}

/*
 * Frees key and deletes value.
 */
void symbol_free(struct hasht_node *n)
{
	log_assert(n);

	free(n->key);
	typeinfo_delete(n->value);
}

/*
 * Given a tree node, get the first subtree with the production rule.
 */
struct tree *get_production(struct tree *n, enum rule r)
{
	log_assert(n);

	if (tree_size(n) != 1 && get_rule(n) == r)
		return n;

	struct list_node *iter = list_head(n->children);
	while (!list_end(iter)) {
		struct tree *t = get_production(iter->data, r);
		if (t)
			return t;
		iter = iter->next;
	}
	return NULL;
}

/*
 * Wrapper to return null if token not found, else return token.
 */
static struct token *get_category(struct tree *t, int target, int before)
{
	struct token *token = get_category_(t, target, before);
	if (token && token->category == target)
		return token;
	else
		return NULL;
}

/*
 * Walks tree returning first token matching category, else null.
 */
static struct token *get_category_(struct tree *t, int target, int before)
{
	log_assert(t);

	if (tree_size(t) == 1) {
		struct token *token = get_token(t->data);
		if (token->category == target || token->category == before)
			return token;
	}

	struct list_node *iter = list_head(t->children);
	while (!list_end(iter)) {
		struct token *token = get_category_(iter->data, target, before);
		if (token && (token->category == target
		              || token->category == before))
			return token;
		iter = iter->next;
	}

	return NULL;
}

/*
 * Returns identifier if found, else null.
 */
char *get_identifier(struct tree *n)
{
	enum yytokentype types[] = {
		IDENTIFIER,
		CHAR,
		BOOL,
		SHORT,
		INT,
		LONG,
		SIGNED,
		UNSIGNED,
		FLOAT,
		DOUBLE,
	};

	for (int i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
		struct token *t = get_category(n, IDENTIFIER, -1);
		if (t)
			return t->text;
	}

	return NULL;
}

/*
 * Returns if pointer is found in tree.
 *
 * Requires it to be found before both identifier and class name so
 * that constructor return types are not made erroneously made
 * pointers because of pointer parameters.
 */
bool get_pointer(struct tree *t)
{
	return get_category(t, '*', IDENTIFIER) && get_category(t, '*', CLASS_NAME);
}

/*
 * Returns size of array if found, 0 if not given, -1 if not array.
 */
int get_array(struct tree *t)
{
	if (get_category(t, '[', INTEGER)) {
		struct token *token = get_category(t, INTEGER, ']');
		return token ? token->ival : 0;
	}
	return -1;
}

/*
 * Returns class name if found, else null.
 */
char *get_class(struct tree *t)
{
	struct token *token = get_category(t, CLASS_NAME, IDENTIFIER);
	if (token)
		return token->text;
	else
		return NULL;
}

static bool get_public(struct tree *t)
{
	return get_category(t, PUBLIC, PRIVATE);
}

static bool get_private(struct tree *t)
{
	return get_category(t, PRIVATE, PUBLIC);
}

/*
 * Returns class name only if like class::something
 */
char *class_member(struct tree *t)
{
	struct tree *prod = NULL;
	if ((prod = get_production(t, DIRECT_DECL4))     /* class::ident */
	    || (prod = get_production(t, DIRECT_DECL5))) /* class::class */
		return get_class(prod);
	return NULL;
}

/*
 * Return address of first symbol in subtree.
 */
struct address get_address(struct tree *t)
{
	/* attempt to get address of identifier */
	char *k = get_identifier(t);
	if (!k)
		log_semantic(t, "identifier not found");

	struct typeinfo *s = scope_search(k);
	if (!s)
		log_semantic(t, "symbol not found for %s", k);

	return s->place;
}

/*
 * Get typeinfo for identifier or literal value.
 */
static struct typeinfo *get_typeinfo(struct tree *t) {
	log_assert(t);

	/* reset base type comparators */
	set_type_comparators();

	/* attempt to get identifier or class */
	char *k = get_identifier(t);
	char *c = get_class(t);

	if (k) {
		/* return function typeinfo */
		return scope_search(k);
	} else if (c) {
		/* return class typeinfo comparator */
		class_type.class.type = c;
		return &class_type;
	} else {
		/* return global basic typeinfo for literal */
		struct token *token = get_token(t->data);

		/* for constants, break and assign place; otherwise return */
		switch (map_type(token->category)) {
		case INT_T:
			return &int_type;
		case FLOAT_T:
			return &float_type;
		case CHAR_T:
			return &char_type;
		case ARRAY_T:
			return &string_type;
		case BOOL_T:
			return &bool_type;
		case VOID_T:
			return &void_type;
		case FUNCTION_T:
		case CLASS_T:
		case UNKNOWN_T:
			return &unknown_type;
		}
	}
	return NULL;
}

/*
 * Require symbol for child 0, left operand.
 */
static struct typeinfo *get_left_type(struct tree *n)
{
	struct tree *n_= child(0);
	struct typeinfo *t = type_check(n_);
	if (t == NULL)
		log_semantic(n_, "left operand undeclared");
	return t;
}

/*
 * Require symbol for child 2, right operand.
 */
static struct typeinfo *get_right_type(struct tree *n)
{
	struct tree *n_= child(2);
	struct typeinfo *t = type_check(n_);
	if (t == NULL)
		log_semantic(n_, "right operand undeclared");
	return t;
}

/*
 * Recursively perform type checking on the relevant expression
 * production rules for the given syntax tree.
 */
struct typeinfo *type_check(struct tree *n)
{
	log_assert(n);

	if (tree_size(n) == 1)
		return get_typeinfo(n);

	enum rule production = get_rule(n);
	switch (production) {
	case LITERAL: {
		/* assign place to constants */
		struct tree *t = child(0);
		log_assert(t);
		struct token *token = get_token(t->data);
		log_assert(token);
		struct typeinfo *v = typeinfo_copy(get_typeinfo(t));
		log_assert(v);
		v->token = token;

		/* constants are only inserted on first appearance */
		if (!scope_search(token->text))
			symbol_insert(token->text, v, t, NULL, true);

		return v;
	}
	case INITIALIZER: {
		/* initialization of simple variable */
		char *k = get_identifier(n->parent);
		if (k == NULL)
			log_semantic(n, "could not get identifier in init");

		struct typeinfo *l = scope_search(k);
		if (l == NULL)
			log_semantic(n, "could not get symbol for %s in init", k);

		struct typeinfo *r = type_check(child(0));
		if (r == NULL)
			log_semantic(n, "symbol %s undeclared", k);

		if (typeinfo_compare(l, r)) {
			log_check("initialized %s", k);
			return l;
		} else if (l->base == CLASS_T
		           && (strcmp(l->class.type, "string") == 0)
		           && r->base == CHAR_T && r->pointer) {
			log_check("initialized std::string %s with string literal", k);
			return l;
		} else {
			log_semantic(n, "could not initialize %s with given type", k);
		}
	}
	case INIT_LIST: {
		/* arrays get lists of initializers, check that each
		   type matches the array's element type */
		char *k = get_identifier(n->parent->parent);
		if (k == NULL)
			log_semantic(n, "could not get identifier in initializer list");

		struct typeinfo *l = scope_search(k);
		if (l == NULL)
			log_semantic(n, "could not get symbol for %s in initializer list", k);

		if (l->base != ARRAY_T)
			log_semantic(n, "initializer list assignee %s was not an array", k);

		struct list_node *iter = list_head(n->children);
		size_t items = 0;
		while (!list_end(iter)) {
			++items;
			struct typeinfo *elem = type_check(iter->data);
			if (!typeinfo_compare(l->array.type, elem))
				log_semantic(n, "initializer item did not match %s element type", k);
			if (items > l->array.size)
				log_semantic(n, "array %k size %zu exceeded by initializer list", k, l->array.size);
			iter = iter->next;
		}
		log_check("initialized %s with list", k);
		return l;
	}
	case NEW_EXPR: {
		/* new operator */
		struct tree *type_spec = get_production(n, TYPE_SPEC_SEQ);
		if (type_spec == NULL)
			log_semantic(n, "new operator missing type spec");

		struct typeinfo *type = typeinfo_copy(type_check(tree_index(type_spec, 0)));
		type->pointer = true;

		if (type->base == CLASS_T) {
			size_t scopes = list_size(yyscopes);

			char *c = type->class.type;
			struct typeinfo *class = scope_search(c);
			if (class) {
				log_debug("pushing class scopes");
				scope_push(class->class.public);
				scope_push(class->class.private);
			}

			struct typeinfo *l = scope_search(c);
			if (l == NULL)
				log_semantic(n, "could not find constructor for %s", c);

			struct typeinfo *r = typeinfo_copy(l);
			r->function.parameters = list_new(NULL, NULL);
			log_assert(r->function.parameters);

			struct tree *expr_list = get_production(n, EXPR_LIST);
			if (expr_list) {
				struct list_node *iter = list_head(expr_list->children);
				while (!list_end(iter)) {
					struct typeinfo *t = type_check(iter->data);
					if (t == NULL)
						log_semantic(iter->data, "could not get type for parameter to %s constructor", c);
					list_push_back(r->function.parameters, t);
					iter = iter->next;
				}
			}

			if (!typeinfo_compare(l, r))
				log_semantic(n, "new operator types mismatched");

			list_free(r->function.parameters);
			free(r);

			/* pop newly pushed scopes */
			log_debug("popping scopes");
			while (list_size(yyscopes) != scopes)
				scope_pop();
			log_check("new operator");
		}

		return type;
	}
	case DELETE_EXPR1:
	case DELETE_EXPR2: {
		/* delete operator */
		struct typeinfo *t = type_check(child(1));
		if (t == NULL)
			log_semantic(n, "symbol %s undeclared", get_identifier(n));

		if (!t->pointer)
			log_semantic(n, "delete operator expected a pointer");

		return NULL;
	}
	case ASSIGN_EXPR: {
		/* assignment operator */
		char *k = get_identifier(child(0));
		if (k == NULL)
			log_semantic(n, "left assignment operand not assignable");

		struct typeinfo *l = get_left_type(n);
		struct typeinfo *r = get_right_type(n);

		switch (get_token(get_node(n, 1))->category) {
		case '=': {
			break;
		}
		case ADDEQ:
		case SUBEQ:
		case MULEQ:
		case DIVEQ: {
			if (!(typeinfo_compare(l, &int_type) || typeinfo_compare(l, &float_type)))
				log_semantic(n, "left operand not an int or double");

			if (!(typeinfo_compare(r, &int_type) || typeinfo_compare(r, &float_type)))
				log_semantic(n, "right operand not an int or double");
			break;
		}
		case MODEQ: {
			if (!typeinfo_compare(l, &int_type) || !typeinfo_compare(r, &int_type))
				log_semantic(n, "modulo operand not an integer");
			break;
		}
		case SREQ:
		case SLEQ:
		case ANDEQ:
		case XOREQ:
		case OREQ: {
			log_semantic(n, "compound bitwise equality operator unsupported");
		}
		}

		if (typeinfo_compare(l, r)) {
			log_check("assigned %s", k);
			return l;
		} else if (l->base == CLASS_T
		           && (strcmp(l->class.type, "string") == 0)
		           && r->base == CHAR_T && r->pointer) {
			log_check("std::string %s assigned with string literal", k);
			return l;
		} else {
			log_semantic(n, "could not assign to %s", k);
		}
	}
	case EQUAL_EXPR:
	case NOTEQUAL_EXPR: {
		struct typeinfo *l = get_left_type(n);
		struct typeinfo *r = get_right_type(n);

		if (!typeinfo_compare(l, r))
			log_semantic(n, "equality operands don't match");

		log_check("equality comparison");
		return &bool_type;
	}
	case REL_LT: /* < */
	case REL_GT: /* > */
	case REL_LTEQ: /* <= */
	case REL_GTEQ: /* >= */ {
		struct typeinfo *l = get_left_type(n);
		if (!(typeinfo_compare(l, &int_type) || typeinfo_compare(l, &float_type)))
			log_semantic(n, "left operand not an int or double");

		struct typeinfo *r = get_right_type(n);
		if (!(typeinfo_compare(r, &int_type) || typeinfo_compare(r, &float_type)))
			log_semantic(n, "right operand not an int or double");

		if (!typeinfo_compare(l, r))
			log_semantic(n, "could not order operands");

		log_check("order comparison");
		return &bool_type;
	}
	case ADD_EXPR:  /* + */
	case SUB_EXPR:  /* - */
	case MULT_EXPR:  /* * */
	case DIV_EXPR: { /* / */
		struct typeinfo *l = get_left_type(n);
		if (!(typeinfo_compare(l, &int_type) || typeinfo_compare(l, &float_type)))
			log_semantic(n, "left operand not an int or double");

		struct typeinfo *r = get_right_type(n);
		if (!(typeinfo_compare(r, &int_type) || typeinfo_compare(r, &float_type)))
			log_semantic(n, "right operand not an int or double");

		if (!typeinfo_compare(l, r))
			log_semantic(n, "could not perform arithmetic on operands");

		log_check("binary arithmetic");
		return l;
	}
	case MOD_EXPR: { /* modulo operator */
		struct typeinfo *l = get_left_type(n);
		struct typeinfo *r = get_right_type(n);

		if (!typeinfo_compare(l, &int_type) || !typeinfo_compare(r, &int_type))
			log_semantic(n, "modulo operand not an integer");

		log_check("modulo arithmetic");
		return l;
	}
	case AND_EXPR:
	case XOR_EXPR:
	case OR_EXPR: {
		log_semantic(n, "C++ bitwise operation unsupported in 120++");
	}
	case LOGICAL_AND_EXPR: /* && */
	case LOGICAL_OR_EXPR:  /* || */ {
		struct typeinfo *l = get_left_type(n);
		if (!(typeinfo_compare(l, &int_type) || typeinfo_compare(l, &bool_type)))
			log_semantic(n, "left operand not an int or bool");

		struct typeinfo *r = get_right_type(n);
		if (!(typeinfo_compare(r, &int_type) || typeinfo_compare(r, &bool_type)))
			log_semantic(n, "right operand not an int or bool");

		log_check("logical comparison");
		return &bool_type;
	}
	case POSTFIX_ARRAY_INDEX: {
		/* array indexing: check identifier is an array and index is an int */
		char *k = get_identifier(n);

		struct typeinfo *l = scope_search(k);
		if (l == NULL)
			log_semantic(n, "array %s not declared", k);
		if (l->base != ARRAY_T)
			log_semantic(n, "trying to index non array symbol %s", k);

		struct typeinfo *r = get_right_type(n);
		if (!typeinfo_compare(&int_type, r))
			log_semantic(n, "array index not an integer");

		log_check("%s[index]", k);
		return l->array.type;
	}
	case POSTFIX_CALL: {
		/* function invocation: build typeinfo list from
		   EXPR_LIST, recursing on each item */
		struct typeinfo *l = get_left_type(n);
		struct typeinfo *r = typeinfo_copy(l);

		r->function.parameters = list_new(NULL, NULL);
		struct tree *iter = get_production(n, EXPR_LIST);
		while (iter && get_rule(iter) == EXPR_LIST) {
			struct typeinfo *t = type_check(iter);
			if (t == NULL)
				log_semantic(iter, "symbol for parameter undeclared");
			list_push_front(r->function.parameters, t);
			iter = tree_index(iter, 0);
		}

		if (!typeinfo_compare(l, r)) {
			log_semantic(n, "function invocation did not match signature");
		}

		list_free(r->function.parameters);
		free(r);

		log_check("function invocation");
		return typeinfo_return(l);
	}
	case POSTFIX_DOT_FIELD: {
		/* class_instance.field access */
		char *k = get_identifier(n);
		char *f = get_identifier(child(2));
		log_assert(k && f);

		struct typeinfo *l = scope_search(k);
		if (l == NULL)
			log_semantic(n, "symbol %s undeclared", k);
		if (l->base != CLASS_T || l->pointer)
			log_semantic(n, "expected %s to be a class instance", k);

		char *c = l->class.type;
		struct typeinfo *class = scope_search(c);
		if (class == NULL)
			log_semantic(n, " symbol %s undeclared", c);

		struct typeinfo *r = hasht_search(class->class.public, f);
		if (r == NULL)
			log_semantic(n, "field %s not in public scope of %s", f, c);

		log_check("%s.%s", k, f);
		return typeinfo_return(r);
	}
	case POSTFIX_ARROW_FIELD: {
		/* class_ptr->field access */
		char *k = get_identifier(n);
		char *f = get_identifier(child(2));
		log_assert(k && f);

		struct typeinfo *l = scope_search(k);
		if (l == NULL)
			log_semantic(n, "symbol %s undeclared", k);
		if (l->base != CLASS_T || !l->pointer)
			log_semantic(n, "expected %s to be a class pointer", k);

		char *c = l->class.type;
		struct typeinfo *class = scope_search(c);
		if (class == NULL)
			log_semantic(n, "symbol %s undeclared", c);

		struct typeinfo *r = hasht_search(class->class.public, f);
		if (r == NULL)
			log_semantic(n, "field %s not in public scope of %s", f, c);

		log_check("%s->%s", k, f);
		return typeinfo_return(r);
	}
	case POSTFIX_PLUSPLUS:  /* i++ */
	case POSTFIX_MINUSMINUS: /* i-- */ {
		struct typeinfo *t = type_check(child(0));
		if (!typeinfo_compare(t, &int_type))
			log_semantic(n, "operand to postfix ++/-- not an int");

		log_check("postfix ++/--");
		return t;
	}
	case UNARY_PLUSPLUS: /* ++i */
	case UNARY_MINUSMINUS: /* --i */ {
		struct typeinfo *t = type_check(child(1));
		if (t == NULL)
			log_semantic(n, "symbol undeclared");
		if (!typeinfo_compare(t, &int_type))
			log_semantic(n, "operand to prefix ++/-- not an int");

		log_check("prefix ++/--");
		return t;
	}
	case UNARY_STAR: {
		/* dereference operator */
		char *k = get_identifier(n);
		log_assert(k);

		struct typeinfo *t = scope_search(k);
		if (t == NULL)
			log_semantic(n, "symbol %s undeclared", k);

		if (!t->pointer)
			log_semantic(n, "cannot dereference non-pointer %s", k);

		struct typeinfo *copy = typeinfo_copy(t);
		copy->pointer = false;

		log_check("*%s", k);
		return copy;
	}
	case UNARY_AMPERSAND: {
		/* address operator */
		char *k = get_identifier(n);
		log_assert(k);

		struct typeinfo *t = scope_search(k);
		if (t == NULL)
			log_semantic(n, "symbol %s undeclared", k);

		if (t->pointer)
			log_semantic(n, "double pointers unsupported in 120++");

		struct typeinfo *copy = typeinfo_copy(t);
		copy->pointer = true;

		log_check("&%s", k);
		return copy;
	}
	case UNARY_PLUS:
	case UNARY_MINUS:
	case UNARY_NOT:
	case UNARY_TILDE: {
		struct typeinfo *t = type_check(child(1));
		if (t == NULL)
			log_semantic(n, "symbol undeclared");

		switch (get_token(get_node(n, 0))->category) {
		case '+':
		case '-':
			if (!typeinfo_compare(t, &int_type))
				log_semantic(n, "unary + or - operand not an int");

			log_check("unary + or -");
			return t;
		case '!':
			if (!(typeinfo_compare(t, &int_type) || typeinfo_compare(t, &bool_type)))
				log_semantic(n, "! operand not an int or bool");

			log_check("logical not");
			return &bool_type;
		case '~':
			log_semantic(n, "destructors not yet supported");
		}
	}
	case UNARY_SIZEOF_EXPR:
	case UNARY_SIZEOF_TYPE: {
		/* sizeof keyword returns an int */
		return &int_type;
	}
	case SHIFT_LEFT: {
		/* << only used for puts-to IO */
		if (!(libs.usingstd && (libs.fstream || libs.iostream)))
			log_semantic(n, "<< can only be used with std streams in 120++");

		/* recurse on items of shift expression */
		struct list_node *iter = list_head(n->children);
		struct typeinfo *ret = NULL;
		while (!list_end(iter)) {
			struct typeinfo *t = type_check(iter->data);
			if (t == NULL)
				log_semantic(iter->data, "symbol undeclared");

			/* ensure leftmost child is of type std::ofstream */
			if (iter == list_head(n->children)) {
				if (!(t->base == CLASS_T && (strcmp(t->class.type, "ofstream") == 0)))
					log_semantic(iter->data, "leftmost << operand not a ofstream");
				/* return leftmost type as result of << */
				ret = t;
			} else if (!(typeinfo_compare(t, &int_type)
			             || typeinfo_compare(t, &float_type)
			             || typeinfo_compare(t, &bool_type)
			             || typeinfo_compare(t, &char_type)
			             || typeinfo_compare(t, &string_type)
			             || (t->base == CLASS_T && (strcmp(t->class.type, "string") == 0)))) {
				log_semantic(iter->data, "a << operand is not an appropriate type");
			}
			iter = iter->next;
		}

		log_check("<<");
		return ret;
	}
	case SHIFT_RIGHT: {
		/* << only used for gets-from IO */
		if (!(libs.usingstd && (libs.fstream || libs.iostream)))
			log_semantic(n, ">> can only be used with std streams in 120++");

		struct tree *l = child(0);
		struct tree *r = child(1);
		char *cin = get_identifier(l);
		if (strcmp(cin, "cin") != 0)
			log_semantic(l, "left operand of >> is not cin");

		struct typeinfo *t = type_check(r);
		if (t == NULL)
			log_semantic(r, "symbol undeclared");
		if (!(typeinfo_compare(t, &int_type)
		      || typeinfo_compare(t, &float_type)
		      || typeinfo_compare(t, &char_type)
		      || typeinfo_compare(t, &string_type)
		      || (t->base == CLASS_T && (strcmp(t->class.type, "string") == 0))))
			log_semantic(r, "right operand of >> is not int, double, char, char *, or std::string");

		log_check(">>");
		return NULL;

	}
	case CTOR_FUNCTION_DEF:
	case FUNCTION_DEF: {
		/* manage scopes for function recursion */
		size_t scopes = list_size(yyscopes);

		/* retrieve class scopes */
		struct typeinfo *class = scope_search(class_member(n));
		if (class) {
			log_debug("pushing class scopes");
			scope_push(class->class.public);
			scope_push(class->class.private);
		}

		/* retrieve function scope */
		char *k = (production == CTOR_FUNCTION_DEF)
			? get_class(n) /* ctor function name is class name */
			: get_identifier(n);

		struct typeinfo *function = scope_search(k);
		if (function == NULL)
			log_semantic(n, "symbol %s undeclared", k);

		log_debug("pushing function scope");
		scope_push(function->function.symbols);

		/* check return type of function */
		struct tree *jump = get_production(n, RETURN_STATEMENT);
		struct typeinfo *ret = NULL;
		if (production == CTOR_FUNCTION_DEF)
			/* constructor always returns class */
			ret = typeinfo_return(function);
		else if (jump == NULL || tree_size(jump) == 2)
			/* no or empty return */
			ret = &void_type;
		else
			/* recurse on return value */
			ret = type_check(tree_index(jump, 1));

		if (!typeinfo_compare(ret, typeinfo_return(function))
		    /* accept void return for 0 if expecting int */
		    && (typeinfo_compare(ret, &int_type)
		        && typeinfo_compare(typeinfo_return(function), &void_type))) {
			log_semantic(n, "return value of wrong type for function %s", k);
		}

		log_check("function %s return type", k);

		/* recursive type check of children while in subscope(s) */
		struct list_node *iter = list_head(n->children);
		while (!list_end(iter)) {
			type_check(iter->data);
			iter = iter->next;
		}

		log_debug("popping scopes");
		while (list_size(yyscopes) != scopes)
			scope_pop();

		return function;
	}
	case EXPR_LIST: {
		/* recurse into expression lists */
		struct tree *c = child(0);
		if (c && get_rule(c) == EXPR_LIST)
			return type_check(child(1));
		else
			return type_check(child(0));
	}
	default: {
		/* recursive search for non-expressions */
		struct list_node *iter = list_head(n->children);
		while (!list_end(iter)) {
			type_check(iter->data);
			iter = iter->next;
		}

	}
	}
	return NULL;
}

/*
 * Recursively handles nodes, processing SIMPLE_DECL and
 * FUNCTION_DEF for symbols.
 */
static bool handle_node(struct tree *n, int d)
{
	switch (get_rule(n)) {
	case SIMPLE_DECL: { /* variable and function declarations */
		handle_init_list(typeinfo_new(n), child(1));
		return false;
	}
	case CTOR_FUNCTION_DEF: { /* constructor definition */
		handle_function(typeinfo_new(n), n, get_class(n));
		return false;
	}
	case FUNCTION_DEF: { /* function or member definition */
		handle_function(typeinfo_new(n), n, get_identifier(n));
		return false;
	}
	case CLASS_SPEC: { /* class declaration */
		handle_class(typeinfo_new(n), n);
		return false;
	}
	default: /* rule did not provide a symbol, so recurse on children */
		return true;
	}
}

/*
 * Handles an init declarator recursively.
 *
 * Works for basic types, pointers, arrays, and function declarations.
 */
static void handle_init(struct typeinfo *v, struct tree *n)
{
	char *k = get_identifier(n);

	switch (get_rule(n)) {
	case INIT_DECL:
	case UNARY_STAR:
	case DECL2:
	case MEMBER_DECL1:
	case MEMBER_DECLARATOR1: {
		/* recurse if necessary (for pointers) */
		struct list_node *iter = list_head(n->children);
		while (!list_end(iter)) {
			if (tree_size(iter->data) != 1
			    && (get_rule(iter->data) != INITIALIZER)) {
				handle_init(v, iter->data);
				return;
			}
			iter = iter->next;
		}
		/* might not recurse */
		break;
	}
	case DIRECT_DECL2: { /* function declaration */
		v = typeinfo_new_function(n, v, false);
		break;
	}
	case DIRECT_DECL3: { /* class constructor */
		k = get_class(n);

		v->base = CLASS_T;
		v->class.type = get_class(n);
		v = typeinfo_new_function(n, v, false);
		break;
	}
	case DIRECT_DECL6: { /* array with size */
		v = typeinfo_new_array(n, v);
		if (v->array.size < 1)
			log_semantic(n, "bad array initializer size");
		break;
	}
	default:
		log_semantic(n, "unsupported init declaration");
	}

	if (k && v) {
		symbol_insert(k, v, n, NULL, false);
	} else {
		log_semantic(n, "failed to get init declarator symbol");
	}
}

/*
 * Handles lists of init declarators recursively.
 */
static void handle_init_list(struct typeinfo *v, struct tree *n)
{
	enum rule r = get_rule(n);
	if (r == INIT_DECL_LIST || r == MEMBER_SPEC1 || r == MEMBER_DECL_LIST2) {
		/* recurse through lists of declarators */
		struct list_node *iter = list_head(n->children);
		while (!list_end(iter)) {
			handle_init_list(v, iter->data);
			iter = iter->next;
		}
	} else if (r == MEMBER_SPEC2) {
		/* begining of class access specifier tree */
		if (get_public(n)) {
			log_debug("creating and pushing public class scope");
			v->class.public = hasht_new(8, true, NULL, NULL, &symbol_free);
			scope_push(v->class.public);
		} else if (get_private(n)) {
			log_debug("creating and pushing private class scope");
			v->class.private = hasht_new(8, true, NULL, NULL, &symbol_free);
			scope_push(v->class.private);
		} else {
			log_semantic(n, "unrecognized class access specifier");
		}

		handle_init_list(v, child(1));

		log_debug("class scope had %zu symbols", hasht_used(list_back(yyscopes)));
		log_debug("popping scope");
		scope_pop();
	} else if (r == MEMBER_DECL1) {
		/* recurse to end of class declaration */
		handle_init_list(typeinfo_new(n), child(1));
	} else {
		/* process a single declaration with copy of type */
		struct typeinfo *c = typeinfo_copy(v);
		c->pointer = get_pointer(n); /* for pointers in lists */
		handle_init(c, n);
	}
}

/*
 * Handles function definitions, recursing with handle_node().
 */
static void handle_function(struct typeinfo *t, struct tree *n, char *k)
{
	struct typeinfo *v = typeinfo_new_function(n, t, true);

	size_t scopes = list_size(yyscopes);

	struct typeinfo *class = scope_search(class_member(n));
	if (class) {
		log_debug("pushing class scopes");
		scope_push(class->class.public);
		scope_push(class->class.private);
	}

	symbol_insert(k, v, n, v->function.symbols, false);

	/* setup local region and offset */
	enum region region_ = region;
	size_t offset_ = offset;
	region = LOCAL_R;
	offset = v->function.param_size;

	/* recurse on children while in subscope */
	log_debug("pushing function scope");
	scope_push(v->function.symbols);
	tree_traverse(get_production(n, COMPOUND_STATEMENT), 0, &handle_node, NULL, NULL);

	log_debug("function scope had %zu symbols", hasht_used(v->function.symbols));
	log_debug("popping scopes");
	while (list_size(yyscopes) != scopes)
		scope_pop();

	/* restore region and offset */
	region = region_;
	offset = offset_;
}

/*
 * Handles a PARAM_DECL1 or a PARAM_DECL3 rules.
 *
 * Works for basic types, pointers, and arrays. If given a scope hash
 * table and able to find an indentifier, inserts into the scope. If
 * given a parameters list, will insert into the list.
 */
static void handle_param(struct typeinfo *v, struct tree *n, struct hasht *s, struct list *l)
{
	log_assert(n);
	char *k = get_identifier(n);

	if (tree_size(n) > 3) { /* not a simple type */
		struct tree *t = child(1);
		enum rule r = get_rule(t);

		/* array (with possible size) */
		if (r == DIRECT_ABSTRACT_DECL4 || r == DIRECT_DECL6)
			v = typeinfo_new_array(t, v);
	}

	/* insert into list when declaring */
	if (l && v)
		list_push_back(l, v);

	/* insert into table when defining */
	if (s && k && v) {
		/* assign region and offset */
		struct node *node = n->data;
		v->place.region = node->place.region = region;
		v->place.offset = node->place.offset = offset;
		v->place.type = node->place.type = v;

		hasht_insert(s, k, v);
		log_symbol(k, v);

		offset += typeinfo_size(v);
	}
}

/*
 * Handles an arbitrarily nested list of parameters recursively.
 */
void handle_param_list(struct tree *n, struct hasht *s, struct list *l)
{
	if (tree_size(n) != 1) { /* recurse on list */
		enum rule r = get_rule(n);
		if (r == PARAM_DECL1 || r == PARAM_DECL3) {
			struct typeinfo *v = typeinfo_new(n);
			v->pointer = get_pointer(n); /* for pointers in list */
			handle_param(v, n, s, l);
		} else {
			struct list_node *iter = list_head(n->children);
			while (!list_end(iter)) {
				handle_param_list(iter->data, s, l);
				iter = iter->next;
			}
		}
	}
}

/*
 * Handles a class declaration with public and/or private scopes.
 */
static void handle_class(struct typeinfo *t, struct tree *n)
{
	char *k = get_identifier(n);
	t->base = CLASS_T; /* class definition is still a class */

	symbol_insert(k, t, n, NULL, false);

	/* setup class region and offset */
	enum region region_ = region;
	size_t offset_ = offset;
	region = CLASS_R;
	offset = 0;

	handle_init_list(t, child(1));

	if (t->class.public == NULL) {
		log_debug("creating default public scope for %s", k);
		t->class.public = hasht_new(2, true, NULL, NULL, &symbol_free);
	}

	if (hasht_search(t->class.public, k) == NULL) {
		log_debug("declaring default constructor for %s", k);
		struct typeinfo *ret = typeinfo_new(n);
		ret->base = CLASS_T;
		ret->class.type = k;
		struct typeinfo *ctor = typeinfo_new_function(n, ret, false);
		scope_push(t->class.public);
		symbol_insert(k, ctor, n, NULL, false);
		scope_pop();
	}

	/* restore region and offset */
	region = region_;
	offset = offset_;
}

#undef child
