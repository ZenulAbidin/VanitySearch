/* 
 * Copyright (c) 2020 Fabian van Rissenbeck

* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:

* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.

* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include <immintrin.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

#include "mregexp.h"

#ifndef __SIZE_MAX__
#define __SIZE_MAX__ 4294967296
#endif

static inline unsigned utf8_char_width(uint8_t c)
{
	int a1 = !(128 & c) && 1;
	int a2 = (128 & c) && (64 & c) && !(32 & c);
	int a3 = (128 & c) && (64 & c) && (32 & c) && !(16 & c);
	int a4 = (128 & c) && (64 & c) && (32 & c) && (16 & c) && !(8 & c);

	return a1 * 1 + a2 * 2 + a3 * 3 + a4 * 4;
}

// utf8 valid is extremly slow
static inline bool utf8_valid(const char *s)
{
	const size_t len = strlen(s);

	for (size_t i = 0; i < len;) {
		const unsigned width = utf8_char_width((uint8_t)s[i]);

		if (width == 0) {
			return false;
		}

		if (i + width > len) {
			return false;
		}

		for (unsigned j = 1; j < width; ++j)
			if ((s[i + j] & (128 + 64)) != 128) {
				return false;
			}

		i += width;
	}

	return true;
}

bool mregexp_check_utf8(const char *s)
{
	return utf8_valid(s);
}

static const int utf8_peek_mods[] = {0, 127, 31, 15, 7};
static inline uint32_t utf8_peek(const char *s)
{
	if (*s == 0)
		return 0;

	const unsigned width = utf8_char_width(s[0]);
	size_t ret = 0;

	ret = s[0] & utf8_peek_mods[width];

	for (unsigned i = 1; i < width; ++i) {
		ret <<= 6;
		ret += s[i] & 63;
	}

	return ret;
}

static inline const char *utf8_next(const char *s)
{
	if (*s == 0)
		return NULL;

	const unsigned width = utf8_char_width((uint8_t)s[0]);
	return s + width;
}

union RegexNode;

/* function pointer type used to evaluate if a regex node
 * matched a given string */
typedef bool (*MatchFunc)(union RegexNode *node, const char *orig,
			  const char *cur, const char **next);

typedef struct GenericNode {
	union RegexNode *prev;
	union RegexNode *next;
	MatchFunc match;
} GenericNode;

typedef struct {
	GenericNode generic;
	uint32_t chr;
} CharNode;

typedef struct {
	GenericNode generic;
	uint8_t chr[16]; // SSE2 enables at most 16 contiguous chars ('\0' indicates not available position)
} Char16Node;

typedef struct {
	GenericNode generic;
	union RegexNode *subexp;
	size_t min, max, omax /* original maximum */;
        size_t current;
} QuantNode;

typedef struct {
	GenericNode generic;
	uint32_t first, last;
} RangeNode;

typedef struct {
	GenericNode generic;
	RangeNode *ranges;
	bool negate;
} ClassNode;

typedef struct {
	GenericNode generic;
	union RegexNode *subexp;
	MRegexpMatch cap;
} CapNode;

typedef struct {
	GenericNode generic;
	union RegexNode *left;
	union RegexNode *right;
} OrNode;

typedef union RegexNode {
	GenericNode generic;
	CharNode chr;
        Char16Node chr16;
	QuantNode quant;
	ClassNode cls;
	RangeNode range;
	CapNode cap;
	OrNode or;
} RegexNode;

static bool quant_is_match(RegexNode *node, const char *orig, const char *cur,
			   const char **next);

static bool is_match(RegexNode *node, const char *orig, const char *cur,
		     const char **next)
{
        QuantNode *qnodes, *qnode;
        RegexNode *rnode;
        size_t i = 0, l = 0;
	if (node == NULL) {
		*next = cur;
		return true;
	} else {
try_again:
                qnodes = NULL;
                i = 0;
                l = 0;
		bool matches1 = ((node->generic.match)(node, orig, cur, next));
                bool matches2 = is_match(node->generic.next, orig, *next, next);
                bool matches = matches1 && matches2;
                /* matches2 will fail if the first match was a quantity match against an empty string.
                 * So check for that here.
                 */
                if (!matches2 && node->generic.match == quant_is_match &&
                        node->quant.max == node->quant.current) {
                    matches2 = is_match(node->generic.next, orig, cur, next);
                    matches = matches1 && matches2;
                }
                if (!matches) {
                    /* Look for the most recent value quant node and try to match
                     * it again but with one less character */
                    rnode = node;
                    while (true) {
                        if (rnode->generic.match == quant_is_match) {
                            qnode = &rnode->quant;
                            qnodes = qnode;
                            ++i;
                            ++l;
                        }
                        /* Finite state automations have the node immediately previously
                         * used, as the next node. */
                        if (rnode->generic.next != NULL) {
                            rnode = rnode->generic.next; 
                        }
                        else {
                            break;
                        }
                    }
                    for (i = l; i > 0; i--) {
                        /* i is size_t, so it can't go negative. That's why it's not allowed to go below 1
                         * and to get the correct node we just subtract 1. */
                        qnode = qnodes + (i-1);
                        /* If there is a quant node that has characters to give away
                         * (a pun on the greediness of quantifier matching),
                         * go back to that matching but with one less character.
                         */
                        if (qnode->max >= qnode->min && qnode->current < qnode->max - qnode->min) {
                            qnode->current++;
                            goto try_again;
                        }
                        else {
                            //qnode->current = 0;
                        }
                    }
                    return false; /* No quant nodes with chars to give away. */
                }
                else {
                    return true;
                }
	}
}

static bool char_is_match(RegexNode *node, const char *orig, const char *cur,
			  const char **next)
{
	if (*cur == 0) {
		return false;
	}

	*next = cur + 1;
	return node->chr.chr == *cur;
}

// It technically works but it does not result in a performance improvement.
// Intel has no instruction for "AND all bytes inside a single XMM" forcing
// me to make many unnecessary OR instructions...
static bool char16_is_match(RegexNode *node, const char *orig, const char *cur,
			  const char **next)
{
        Char16Node char16 = node->chr16;
        int result;
        __m128i xmm0, xmm1, xmm2;

        *next = cur+16;
        xmm0 = _mm_loadu_si128((__m128i*) char16.chr);
        xmm1 = _mm_loadu_si128((__m128i*) cur);
        xmm2 = _mm_cmpeq_epi8(xmm0, xmm1);
        result = xmm2[0] & xmm2[1] & xmm2[2] & xmm2[3] & xmm2[4] & xmm2[5] & xmm2[6]
            & xmm2[7] & xmm2[8] & xmm2[9] & xmm2[10] & xmm2[11] & xmm2[12]
            & xmm2[13] & xmm2[14] & xmm2[15];

        return result;
}

static bool start_is_match(RegexNode *node, const char *orig, const char *cur,
			   const char **next)
{
	*next = cur;
	return true;
}

static bool anchor_begin_is_match(RegexNode *node, const char *orig,
				  const char *cur, const char **next)
{
	*next = cur;
	return strlen(orig) == strlen(cur);
}

static bool anchor_end_is_match(RegexNode *node, const char *orig,
				const char *cur, const char **next)
{
	*next = cur;
	return strlen(cur) == 0;
}

static bool any_is_match(RegexNode *node, const char *orig, const char *cur,
			 const char **next)
{
	if (*cur) {
		*next = cur + 1;
		return true;
	}

	return false;
}

static bool quant_is_match(RegexNode *node, const char *orig, const char *cur,
			   const char **next)
{
	QuantNode *quant = (QuantNode *) node;
        /* First reset the maximum back to the original value */
        quant->max = quant->omax;

        /* Required for backtracking - if the maximum allowed chars is less
         * than the number of characters left, then lower the maximum to that.
         */
        size_t lcur = strlen(cur);
	size_t matches = 0;
        if (quant->max > lcur) quant->max = lcur;


        //if (quant->max == quant->current) return true;
        else if (quant->current > quant->max - quant->min) return false;
	while (is_match(quant->subexp, orig, cur, next)) {
		matches++;
		cur = *next;

		if (matches >= quant->max - quant->current)
			break;
	}

	*next = cur;
	return matches >= quant->min;
}

static bool class_is_match(RegexNode *node, const char *orig, const char *cur,
			   const char **next)
{
	ClassNode *cls = (ClassNode *)node;

	if (*cur == 0)
		return false;

	const uint32_t chr = *cur;
	*next = cur + 1;

	bool found = false;
	for (RangeNode *range = cls->ranges; range != NULL;
	     range = (RangeNode *)range->generic.next) {
		if (chr >= range->first && chr <= range->last) {
			found = true;
			break;
		}
	}

	if (cls->negate)
		found = !found;

	return found;
}

static bool cap_is_match(RegexNode *node, const char *orig, const char *cur,
			 const char **next)
{
	CapNode *cap = (CapNode *)node;

	if (is_match(cap->subexp, orig, cur, next)) {
		cap->cap.match_begin = cur - orig;
		cap->cap.match_end = (*next) - orig;
		return true;
	}

	return false;
}

static bool or_is_match(RegexNode *node, const char *orig, const char *cur,
	const char **next)
{
	OrNode *or = (OrNode *)node;

	if (or->generic.next != NULL) {
		or->right = or->generic.next;
		or->generic.next = NULL;
	}

	if (is_match(or->left, orig, cur, next) && or->left != NULL) {
		return true;
	}

	return is_match(or->right, orig, cur, next) && or->right != NULL;
}

/* Global error value with callback address */
struct {
	MRegexpError err;
	const char *s;
	jmp_buf buf;
} CompileException;

/* set global error value to the default value */
static inline void clear_compile_exception(void)
{
	CompileException.err = MREGEXP_OK;
	CompileException.s = NULL;
}

/* set global error value and jump back to the exception handler */
static void throw_compile_exception(MRegexpError err, const char *s)
{
	CompileException.err = err;
	CompileException.s = s;
	longjmp(CompileException.buf, 1);
}

static size_t calc_compiled_escaped_len(const char *s, const char **leftover)
{
	if (*s == 0)
		throw_compile_exception(MREGEXP_UNEXPECTED_EOL, s);

	const uint32_t chr = *s;
	*leftover = s + 1;

	switch (chr) {
	case 's':
		return 5;

	case 'S':
		return 5;

	case 'd':
		return 2;

	case 'D':
		return 2;

	case 'w':
		return 5;

	case 'W':
		return 5;

	default:
		return 1;
	}
}

static const size_t calc_compiled_class_len(const char *s,
					    const char **leftover)
{
	if (*s == '^')
		s++;

	size_t ret = 1;

	while (*s && *s != ']') {
		uint32_t chr = *s;
		s = s + 1;
		if (chr == '\\') {
			s = s + 1;
		}

		if (*s == '-' && s[1] != ']') {
			s++;
			chr = *s;
			s = s + 1;

			if (chr == '\\')
				s = s + 1;
		}

		ret++;
	}

	if (*s == ']') {
		s++;
		*leftover = s;
	} else {
		throw_compile_exception(MREGEXP_INVALID_COMPLEX_CLASS, s);
	}

	return ret;
}

/* get required amount of memory in amount of nodes
 * to compile regular expressions */
static const size_t calc_compiled_len(const char *s)
{
	if (*s == 0) {
		return 1;
	} else {
		const uint32_t chr = *s;
		size_t ret = 0;
		s = s + 1;

		switch (chr) {
		case '{': {
			const char *end = strstr(s, "}");

			if (end == NULL)
				throw_compile_exception(
					MREGEXP_INVALID_COMPLEX_QUANT, s);

			s = end + 1;
			ret = 1;
			break;
		}

		case '\\':
			ret = calc_compiled_escaped_len(s, &s);
			break;

		case '[':
			ret = calc_compiled_class_len(s, &s);
			break;

		default:
			ret = 1;
			break;
		}

		return ret + calc_compiled_len(s);
	}
}

static void append_quant(RegexNode **prev, RegexNode *cur, size_t min,
			 size_t max, const char *re)
{
	cur->generic.match = quant_is_match;
	cur->generic.next = NULL;
	cur->generic.prev = NULL;

	cur->quant.max = max;
	cur->quant.omax = max;
	cur->quant.min = min;
        cur->quant.current = 0;
	cur->quant.subexp = *prev;

	*prev = (*prev)->generic.prev;
	if (*prev == NULL)
		throw_compile_exception(MREGEXP_EARLY_QUANTIFIER, re);

	cur->quant.subexp->generic.next = NULL;
	cur->quant.subexp->generic.prev = NULL;
}

static inline bool is_digit(uint32_t c)
{
	return c >= '0' && c <= '9';
}

static inline size_t parse_digit(const char *s, const char **leftover)
{
	size_t ret = 0;

	while (*s) {
		uint32_t chr = *s;

		if (is_digit(chr)) {
			ret *= 10;
			ret += chr - '0';
			s = s + 1;
		} else {
			break;
		}
	}

	*leftover = s;
	return ret;
}

/* parse complex quantifier of format {m,n} 
 * valid formats: {,} {m,} {,n} {m} {m,n} */
static void parse_complex_quant(const char *re, const char **leftover,
				size_t *min_p, size_t *max_p)
{
	if (*re == 0)
		throw_compile_exception(MREGEXP_INVALID_COMPLEX_QUANT, re);

	uint32_t tmp = *re;
	size_t min = 0, max = __SIZE_MAX__;

	if (is_digit(tmp)) {
		min = parse_digit(re, &re);
	} else if (tmp != ',') {
		throw_compile_exception(MREGEXP_INVALID_COMPLEX_QUANT, re);
	}

	tmp = *re;

	if (tmp == ',') {
		re = re + 1;
		if (is_digit(utf8_peek(re)))
			max = parse_digit(re, &re);
		else
			max = __SIZE_MAX__;
	} else {
		max = min;
	}

	tmp = *re;
	if (tmp == '}') {
		*leftover = re + 1;
		*min_p = min;
		*max_p = max;
	} else {
		throw_compile_exception(MREGEXP_INVALID_COMPLEX_QUANT, re);
	}
}

/* append character class to linked list of nodes with
 * ranges given as optional arguments. Returns pointer
 * to next */
static RegexNode *append_class(RegexNode *cur, bool negate, size_t n, ...)
{
	cur->cls.negate = negate;
	cur->cls.ranges = (RangeNode *)(n ? cur + 1 : NULL);
	cur->generic.match = class_is_match;
	cur->generic.next = NULL;
	cur->generic.prev = NULL;

	va_list ap;
	va_start(ap, n);
	RegexNode *prev = NULL;
	cur = cur + 1;

	for (size_t i = 0; i < n; ++i) {
		const uint32_t first = va_arg(ap, uint32_t);
		const uint32_t last = va_arg(ap, uint32_t);

		cur->generic.next = NULL;
		cur->generic.prev = prev;

		if (prev)
			prev->generic.next = cur;

		cur->range.first = first;
		cur->range.last = last;

		prev = cur;
		cur = cur + 1;
	}

	va_end(ap);

	return cur;
}

/** compile escaped characters. return pointer to the next free node. */
static RegexNode *compile_next_escaped(const char *re, const char **leftover,
				       RegexNode *cur)
{
	if (*re == 0)
		throw_compile_exception(MREGEXP_UNEXPECTED_EOL, re);

	const uint32_t chr = *re;
	*leftover = re + 1;
	RegexNode *ret = cur + 1;

	switch (chr) {
	case 'n':
		cur->chr.chr = '\n';
		cur->generic.match = char_is_match;
		break;

	case 't':
		cur->chr.chr = '\t';
		cur->generic.match = char_is_match;
		break;

	case 'r':
		cur->chr.chr = '\r';
		cur->generic.match = char_is_match;
		break;

	case 's':
		ret = append_class(cur, false, 4, ' ', ' ', '\t', '\t', '\r',
				   '\r', '\n', '\n');
		break;

	case 'S':
		ret = append_class(cur, true, 4, ' ', ' ', '\t', '\t', '\r',
				   '\r', '\n', '\n');
		break;

	case 'w':
		ret = append_class(cur, false, 4, 'a', 'z', 'A', 'Z', '0', '9',
				   '_', '_');
		break;

	case 'W':
		ret = append_class(cur, true, 4, 'a', 'z', 'A', 'Z', '0', '9',
				   '_', '_');
		break;

	case 'd':
		ret = append_class(cur, false, 1, '0', '9');
		break;

	case 'D':
		ret = append_class(cur, true, 1, '0', '9');
		break;

	default:
		cur->chr.chr = chr;
		cur->generic.match = char_is_match;
		break;
	}

	return ret;
}

static RegexNode *compile_next_complex_class(const char *re,
					     const char **leftover,
					     RegexNode *cur)
{
	cur->generic.match = class_is_match;
	cur->generic.next = NULL;
	cur->generic.prev = NULL;

	if (*re == '^') {
		re++;
		cur->cls.negate = true;
	} else {
		cur->cls.negate = false;
	}

	cur->cls.ranges = NULL;

	cur = cur + 1;
	RegexNode *prev = NULL;

	while (*re && *re != ']') {
		uint32_t first = 0, last = 0;

		first = *re;
		re = re + 1;
		if (first == '\\') {
			if (*re == 0)
				throw_compile_exception(
					MREGEXP_INVALID_COMPLEX_CLASS, re);

			first = *re;
			re = re + 1;
		}

		if (*re == '-' && re[1] != ']' && re[1]) {
			re++;
			last = *re;
			re = re + 1;

			if (last == '\\') {
				if (*re == 0)
					throw_compile_exception(
						MREGEXP_INVALID_COMPLEX_CLASS,
						re);

				last = *re;
				re = re + 1;
			}
		} else {
			last = first;
		}

		cur->range.first = first;
		cur->range.last = last;
		cur->generic.prev = prev;
		cur->generic.next = NULL;

		if (prev == NULL) {
			(cur - 1)->cls.ranges = (RangeNode *)cur;
		} else {
			prev->generic.next = cur;
		}

		prev = cur;
		cur++;
	}

	if (*re == ']') {
		*leftover = re + 1;
		return cur;
	} else {
		throw_compile_exception(MREGEXP_INVALID_COMPLEX_CLASS, re);
		return NULL; // Unreachable
	}
}

static const char *find_closing_par(const char *s)
{
	size_t level = 1;

	for (; *s && level != 0; ++s) {
		if (*s == '\\')
			s++;
		else if (*s == '(')
			level++;
		else if (*s == ')')
			level--;
	}

	if (level == 0)
		return s;
	else
		return NULL;
}

static RegexNode *compile(const char *re, const char *end, RegexNode *nodes);

static RegexNode *compile_next_cap(const char *re, const char **leftover,
				   RegexNode *cur)
{
	cur->cap.cap.match_begin = 0;
	cur->cap.cap.match_end = 0;
	cur->cap.subexp = cur + 1;
	cur->generic.next = NULL;
	cur->generic.prev = NULL;
	cur->generic.match = cap_is_match;

	const char *end = find_closing_par(re);

	if (end == NULL)
		throw_compile_exception(MREGEXP_UNCLOSED_SUBEXPRESSION, re);

	*leftover = end;
	return compile(re, end - 1, cur + 1);
}

static RegexNode *insert_or(RegexNode *cur, RegexNode **prev) {
	cur->generic.match = or_is_match;
	cur->generic.next = NULL;
	cur->generic.prev = NULL;

	// Find last start node
	RegexNode *begin = *prev;

	while (begin->generic.match != start_is_match) {
		begin = begin->generic.prev;
	}

	cur->or.left = begin->generic.next;
	*prev = begin;

	return cur + 1;
}

/* compile next node. returns address of next available node.
 * returns NULL if re is empty */
static RegexNode *compile_next(const char *re, const char **leftover,
			       RegexNode *prev, RegexNode *cur)
{
	if (*re == 0)
		return NULL;

	const uint32_t chr = *re;
	re = re + 1;
	RegexNode *next = cur + 1;

	switch (chr) {
	case '^':
		cur->generic.match = anchor_begin_is_match;
		break;

	case '$':
		cur->generic.match = anchor_end_is_match;
		break;

	case '.':
		cur->generic.match = any_is_match;
		break;

	case '*':
		append_quant(&prev, cur, 0, __SIZE_MAX__, re);
		break;

	case '+':
		append_quant(&prev, cur, 1, __SIZE_MAX__, re);
		break;

	case '?':
		append_quant(&prev, cur, 0, 1, re);
		break;

	case '{': {
		size_t min = 0, max = __SIZE_MAX__;
		const char *leftover = NULL;
		parse_complex_quant(re, &leftover, &min, &max);

		append_quant(&prev, cur, min, max, re);
		re = leftover;
		break;
	}

	case '[':
		next = compile_next_complex_class(re, &re, cur);
		break;

	case '(':
		next = compile_next_cap(re, &re, cur);
		break;

	case '\\':
		next = compile_next_escaped(re, &re, cur);
		break;

	case '|':
		next = insert_or(cur, &prev);
		break;

	default:
		cur->chr.chr = chr;
		cur->generic.match = char_is_match;
		break;
	}

	cur->generic.next = NULL;
	cur->generic.prev = prev;
	prev->generic.next = cur;
	*leftover = re;

	return next;
}

/* compile raw regular expression into a linked list of nodes. return leftover nodes */
static RegexNode *compile(const char *re, const char *end, RegexNode *nodes)
{
	RegexNode *prev = nodes;
	RegexNode *cur = nodes + 1;

	prev->generic.next = NULL;
	prev->generic.prev = NULL;
	prev->generic.match = start_is_match;

	while (cur != NULL && re != NULL && re < end) {
		const char *next = NULL;
		RegexNode *next_node = compile_next(re, &next, prev, cur);

		prev = cur;
		cur = next_node;
		re = next;
	}

	return cur;
}

struct MRegexp {
	RegexNode *nodes;
};


MRegexp *mregexp_compile(const char *re)
{
	clear_compile_exception();
	if (re == NULL) {
		CompileException.err = MREGEXP_INVALID_PARAMS;
		return NULL;
	}

	if (!utf8_valid(re)) {
		CompileException.err = MREGEXP_INVALID_UTF8;
		CompileException.s = NULL;
		return NULL;
	}

	MRegexp *ret = (MRegexp *)calloc(1, sizeof(MRegexp));

	if (ret == NULL) {
		CompileException.err = MREGEXP_FAILED_ALLOC;
		CompileException.s = NULL;
		return NULL;
	}

	RegexNode *nodes = NULL;

	if (setjmp(CompileException.buf)) {
		// Error callback
		free(ret);
		free(nodes);

		return NULL;
	}

	const size_t compile_len = calc_compiled_len(re);
	nodes = (RegexNode *)calloc(compile_len, sizeof(RegexNode));
	compile(re, re + strlen(re), nodes);
	ret->nodes = nodes;

	return ret;
}

MRegexpError mregexp_error(void)
{
	return CompileException.err;
}

bool mregexp_match(MRegexp *re, const char *s, MRegexpMatch *m)
{
        bool matchd = false;
        RegexNode* node;
	clear_compile_exception();

	if (re == NULL || s == NULL || m == NULL) {
		CompileException.err = MREGEXP_INVALID_PARAMS;
		return false;
	}

	m->match_begin = __SIZE_MAX__;
	m->match_end = __SIZE_MAX__;

	for (const char *tmp_s = s; *tmp_s; tmp_s = tmp_s + 1)  {
            const char *next = NULL;
            if (is_match(re->nodes, s, tmp_s, &next)) {
                    m->match_begin = tmp_s - s;
                    m->match_end = next - s;
                    matchd = true;
                    break;
            }
            if (s[0] == '^') {
                // It's never going to match if we start at a later character
                break;
            }
            for (node = re->nodes; node != NULL; node = node->generic.next) {
                if (node->generic.match == quant_is_match) {
                    node->quant.current = 0;
                    node->quant.max = node->quant.omax;
                }
            }
	}


	return matchd;
}

void mregexp_free(MRegexp *re)
{
	if (re == NULL) {
		CompileException.err = MREGEXP_INVALID_PARAMS;
		return;
	}
	free(re->nodes);
	free(re);
}

MRegexpMatch *mregexp_all_matches(MRegexp *re, const char *s, size_t *sz)
{
	MRegexpMatch *matches = NULL;
	size_t offset = 0;
	*sz = 0;

	const char *end = s + strlen(s);
	while (s < end) {
		MRegexpMatch tmp;
		if (mregexp_match(re, s, &tmp)) {
			size_t end = tmp.match_end;
			s = s + end;

			matches = (MRegexpMatch *)realloc(
				matches, (++(*sz)) * sizeof(MRegexpMatch));

			if (matches == NULL)
				return NULL;

			tmp.match_begin += offset;
			tmp.match_end += offset;

			offset += end;
			matches[(*sz) - 1] = tmp;
		} else {
			break;
		}
	}

	return matches;
}

/* calculate amount of capture groups
 * inside a regular expression */
static size_t cap_node_count(RegexNode *nodes)
{
	if (nodes == NULL) {
		return 0;
	} else if (nodes->generic.match == quant_is_match) {
		return cap_node_count(nodes->quant.subexp) +
		       cap_node_count(nodes->generic.next);
	} else if (nodes->generic.match == cap_is_match) {
		return cap_node_count(nodes->quant.subexp) +
		       cap_node_count(nodes->generic.next) + 1;
	} else {
		return cap_node_count(nodes->generic.next);
	}
}

size_t mregexp_captures_len(MRegexp *re)
{
	return cap_node_count(re->nodes);
}

static RegexNode *find_capture_node(RegexNode *node, size_t index)
{
	if (node == NULL) {
		return NULL;
	} else if (node->generic.match == cap_is_match) {
		if (index == 0) {
			return node;
		} else {
			const size_t subexp_len =
				cap_node_count(node->cap.subexp);
			if (index <= subexp_len) {
				return find_capture_node(node->cap.subexp,
							 index - subexp_len);
			} else {
				return find_capture_node(node->generic.next,
							 index - 1 -
								 subexp_len);
			}
		}
	} else if (node->generic.match == quant_is_match) {
		const size_t subexp_len = cap_node_count(node->quant.subexp);
		if (index < subexp_len) {
			return find_capture_node(node->quant.subexp, index);
		} else {
			return find_capture_node(node->generic.next, index);
		}
	} else {
		return find_capture_node(node->generic.next, index);
	}
}

const MRegexpMatch *mregexp_capture(MRegexp *re, size_t index)
{
	CapNode *cap = (CapNode *)find_capture_node(re->nodes, index);

	if (cap == NULL) {
		return NULL;
	}

	return &cap->cap;
}
