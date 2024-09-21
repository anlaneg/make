/* Variable expansion functions for GNU Make.
Copyright (C) 1988-2022 Free Software Foundation, Inc.
This file is part of GNU Make.

GNU Make is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 3 of the License, or (at your option) any later
version.

GNU Make is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "makeint.h"

#include <assert.h>

#include "filedef.h"
#include "job.h"
#include "commands.h"
#include "variable.h"
#include "rule.h"

/* Initially, any errors reported when expanding strings will be reported
   against the file where the error appears.  */
const floc **expanding_var = &reading_file;

/* The next two describe the variable output buffer.
   This buffer is used to hold the variable-expansion of a line of the
   makefile.  It is made bigger with realloc whenever it is too small.
   variable_buffer_length is the size currently allocated.
   variable_buffer is the address of the buffer.

   For efficiency, it's guaranteed that the buffer will always have
   VARIABLE_BUFFER_ZONE extra bytes allocated.  This allows you to add a few
   extra chars without having to call a function.  Note you should never use
   these bytes unless you're _sure_ you have room (you know when the buffer
   length was last checked.  */

#define VARIABLE_BUFFER_ZONE    5

static size_t variable_buffer_length;
char *variable_buffer;

/* Subroutine of variable_expand and friends:
   The text to add is LENGTH chars starting at STRING to the variable_buffer.
   The text is added to the buffer at PTR, and the updated pointer into
   the buffer is returned as the value.  Thus, the value returned by
   each call to variable_buffer_output should be the first argument to
   the following call.  */

/*将string指向的长度为length的字符串。输出到ptr中*/
char *
variable_buffer_output (char *ptr, const char *string, size_t length)
{
    /*新的长度*/
  size_t newlen = length + (ptr - variable_buffer);

  if ((newlen + VARIABLE_BUFFER_ZONE) > variable_buffer_length)
    {
      /*variable buffer需要扩充*/
      size_t offset = ptr - variable_buffer;
      variable_buffer_length = (newlen + 100 > 2 * variable_buffer_length
                                ? newlen + 100
                                : 2 * variable_buffer_length);
      variable_buffer = xrealloc (variable_buffer, variable_buffer_length);
      ptr = variable_buffer + offset;
    }

  /*将string输出到ptr指向的内存里*/
  /*返回填充后的新起始位置*/
  return mempcpy (ptr, string, length);
}

/* Return a pointer to the beginning of the variable buffer.
   This is called from main() and it should never be null afterward.  */

char *
initialize_variable_output ()
{
  /* If we don't have a variable output buffer yet, get one.  */

  if (variable_buffer == 0)
    {
      /*申请variable_buffer空间，设置variable_buffer长度*/
      variable_buffer_length = 200;
      variable_buffer = xmalloc (variable_buffer_length);
      variable_buffer[0] = '\0';
    }

  return variable_buffer;
}

/* Recursively expand V.  The returned string is malloc'd.  */

static char *allocated_variable_append (const struct variable *v);

char *
recursively_expand_for_file (struct variable *v, struct file *file)
{
  char *value;
  const floc *this_var;
  const floc **saved_varp;
  struct variable_set_list *save = 0;
  int set_reading = 0;

  /* Don't install a new location if this location is empty.
     This can happen for command-line variables, builtin variables, etc.  */
  saved_varp = expanding_var;
  if (v->fileinfo.filenm)
    {
      this_var = &v->fileinfo;
      expanding_var = &this_var;
    }

  /* If we have no other file-reading context, use the variable's context. */
  if (!reading_file)
    {
      set_reading = 1;
      reading_file = &v->fileinfo;
    }

  if (v->expanding)
    {
      if (!v->exp_count)
        /* Expanding V causes infinite recursion.  Lose.  */
        OS (fatal, *expanding_var,
            _("Recursive variable '%s' references itself (eventually)"),
            v->name);
      --v->exp_count;
    }

  if (file)
    {
      save = current_variable_set_list;
      current_variable_set_list = file->variables;
    }

  v->expanding = 1;
  if (v->append)
    value = allocated_variable_append (v);
  else
    value = allocated_variable_expand (v->value);
  v->expanding = 0;

  if (set_reading)
    reading_file = 0;

  if (file)
    current_variable_set_list = save;

  expanding_var = saved_varp;

  return value;
}

/* Expand a simple reference to variable NAME, which is LENGTH chars long.  */

#ifdef __GNUC__
__inline
#endif
/*执行给定变量的取值的输出*/
static char *
reference_variable (char *o, const char *name/*变量名称*/, size_t length/*变量名称长度*/)
{
  struct variable *v;
  char *value;

  /*查询变量表*/
  v = lookup_variable (name, length);

  if (v == 0)
      /*未查询到，告警变量未定义*/
    warn_undefined (name, length);

  /* If there's no variable by that name or it has no value, stop now.  */
  if (v == 0 || (*v->value == '\0' && !v->append))
      /*变量不存在或者变量值为空，直接返回output*/
    return o;

  value = (v->recursive ? recursively_expand (v) : v->value);

  /*执行变量输出*/
  o = variable_buffer_output (o, value, strlen (value));

  if (v->recursive)
    free (value);

  return o;
}

/* Scan STRING for variable references and expansion-function calls.  Only
   LENGTH bytes of STRING are actually scanned.  If LENGTH is -1, scan until
   a null byte is found.

   Write the results to LINE, which must point into 'variable_buffer'.  If
   LINE is NULL, start at the beginning of the buffer.
   Return a pointer to LINE, or to the beginning of the buffer if LINE is
   NULL.
 */
char *
variable_expand_string (char *line/*展开后要输出的buffer*/, const char *string/*待展开的内容*/, size_t length)
{
  struct variable *v;
  const char *p, *p1;
  char *save;
  char *o;
  size_t line_offset;

  if (!line)
    /*未指定line,使用variable buffer*/
    line = initialize_variable_output ();
  o = line;

  /*起始位置到variable buffer之间的偏移量*/
  line_offset = line - variable_buffer;

  if (length == 0)
    {
      variable_buffer_output (o, "", 1);
      return variable_buffer;
    }

  /* We need a copy of STRING: due to eval, it's possible that it will get
     freed as we process it (it might be the value of a variable that's reset
     for example).  Also having a nil-terminated string is handy.  */
  save = length == SIZE_MAX ? xstrdup (string) : xstrndup (string, length);
  p = save;

  while (1)
    {
      /* Copy all following uninteresting chars all at once to the
         variable output buffer, and skip them.  Uninteresting chars end
         at the next $ or the end of the input.  */

      p1 = strchr (p, '$');/*在p位置查找$起始位置*/

      /*前半部分原样输出*/
      o = variable_buffer_output (o, p, p1 != 0 ? (size_t) (p1 - p) : strlen (p) + 1);

      if (p1 == 0)
        break;
      p = p1 + 1;/*跳过$符号*/

      /* Dispatch on the char that follows the $.  */

      switch (*p)
        {
        case '$':
        case '\0':
            /*如注释言，$$或者以$结尾，则输出为$符号*/
          /* $$ or $ at the end of the string means output one $ to the
             variable output buffer.  */
          o = variable_buffer_output (o, p1, 1);
          break;

        case '(':
        case '{':
            /*遇到$(,或者遇到${*/
          /* $(...) or ${...} is the general case of substitution.  */
          {
              /*设置开区间标记*/
            char openparen = *p;
            /*由开区间标记，获得闭区间标记*/
            char closeparen = (openparen == '(') ? ')' : '}';
            const char *begp;
            /*闭区间查找起始位置*/
            const char *beg = p + 1;
            char *op;
            char *abeg = NULL;
            const char *end, *colon;

            op = o;
            begp = p;/*指向p地址，以便handle_function可以获知 ”开区间标记”*/
            /*针对函数进行处理*/
            if (handle_function (&op, &begp))
              {
                o = op;
                p = begp;
                break;/*函数处理成功，解析下一句*/
              }

            /* Is there a variable reference inside the parens or braces?
               If so, expand it before expanding the entire reference.  */

            /*针对变量的处理*/
            end = strchr (beg, closeparen);/*查找第一个闭区间标记（这里变量不容许嵌套）*/
            if (end == 0)
                /*报错，没有遇到闭区间*/
              /* Unterminated variable reference.  */
              O (fatal, *expanding_var, _("unterminated variable reference"));
            /*在变量格式之间，查找$符号，这里处理嵌套问题*/
            p1 = lindex (beg, end, '$');
            if (p1 != 0)
              {
                /*变量中有嵌套，在这里进行层次匹配*/
                /* BEG now points past the opening paren or brace.
                   Count parens or braces until it is matched.  */
                int count = 0;
                for (p = beg; *p != '\0'; ++p)
                  {
                    if (*p == openparen)
                        /*遇到区间标记，层次增加*/
                      ++count;
                    else if (*p == closeparen && --count < 0)
                        /*层次减少，如果匹配完成，跳出*/
                      break;
                  }
                /* If COUNT is >= 0, there were unmatched opening parens
                   or braces, so we go to the simple case of a variable name
                   such as '$($(a)'.  */
                if (count < 0)
                  {
                    /*成功匹配了层次，这里执行beg,p之间变量的展开*/
                    abeg = expand_argument (beg, p); /* Expand the name.  */
                    beg = abeg;
                    end = strchr (beg, '\0');
                  }
              }
            else
              /* Advance P to the end of this reference.  After we are
                 finished expanding this one, P will be incremented to
                 continue the scan.  */
              p = end;

            /* This is not a reference to a built-in function and
               any variable references inside are now expanded.
               Is the resultant text a substitution reference?  */
            /*在beg与end之间，查找':'号，考虑$(FOO:A=B)这种情况*/
            colon = lindex (beg, end, ':');
            if (colon)
              {
                /* This looks like a substitution reference: $(FOO:A=B).  */
                const char *subst_beg = colon + 1;/*子串开始*/
                /*查找到'='*/
                const char *subst_end = lindex (subst_beg, end, '=');
                if (subst_end == 0)
                  /* There is no = in sight.  Punt on the substitution
                     reference and treat this as a variable name containing
                     a colon, in the code below.  */
                  colon = 0;
                else
                  {
                    const char *replace_beg = subst_end + 1;/*指向'='后面的字符*/
                    const char *replace_end = end;

                    /* Extract the variable name before the colon
                       and look up that variable.  */
                    v = lookup_variable (beg, colon - beg);
                    if (v == 0)
                        /*未找到上面提到的格式FOO,对应的变量，告警*/
                      warn_undefined (beg, colon - beg);

                    /* If the variable is not empty, perform the
                       substitution.  */
                    if (v != 0 && *v->value != '\0')
                      {
                        char *pattern, *replace, *ppercent, *rpercent;
                        char *value = (v->recursive
                                       ? recursively_expand (v)
                                       : v->value);

                        /* Copy the pattern and the replacement.  Add in an
                           extra % at the beginning to use in case there
                           isn't one in the pattern.  */
                        pattern = alloca (subst_end - subst_beg + 2);
                        *(pattern++) = '%';
                        memcpy (pattern, subst_beg, subst_end - subst_beg);
                        pattern[subst_end - subst_beg] = '\0';

                        replace = alloca (replace_end - replace_beg + 2);
                        *(replace++) = '%';
                        memcpy (replace, replace_beg,
                               replace_end - replace_beg);
                        replace[replace_end - replace_beg] = '\0';

                        /* Look for %.  Set the percent pointers properly
                           based on whether we find one or not.  */
                        ppercent = find_percent (pattern);
                        if (ppercent)
                          {
                            ++ppercent;
                            rpercent = find_percent (replace);
                            if (rpercent)
                              ++rpercent;
                          }
                        else
                          {
                            ppercent = pattern;
                            rpercent = replace;
                            --pattern;
                            --replace;
                          }

                        /*执行字符串替换，并输出*/
                        o = patsubst_expand_pat (o, value, pattern, replace,
                                                 ppercent, rpercent);

                        if (v->recursive)
                          free (value);
                      }
                  }
              }

            if (colon == 0)
                /*这是一个纯粹的变量引用，查找变量取值，并输出到o*/
              /* This is an ordinary variable reference.
                 Look up the value of the variable.  */
                o = reference_variable (o, beg, end - beg);

            free (abeg);
          }
          break;

        default:
            /*变量结尾是空格，解析结束*/
          if (ISSPACE (p[-1]))
            break;

          /* A $ followed by a random char is a variable reference:
             $a is equivalent to $(a).  */
          /*遇到$a格式的变量，按$(a)进行解析*/
          o = reference_variable (o, p, 1);

          break;
        }

      if (*p == '\0')
        break;

      ++p;
    }

  free (save);

  variable_buffer_output (o, "", 1);
  return (variable_buffer + line_offset);
}

/* Scan LINE for variable references and expansion-function calls.
   Build in 'variable_buffer' the result of expanding the references and calls.
   Return the address of the resulting string, which is null-terminated
   and is valid only until the next time this function is called.  */

char *
variable_expand (const char *line)
{
    /*展开参数line中包含的变量*/
  return variable_expand_string (NULL/*展开后要输出的buffer*/, line, SIZE_MAX);
}

/* Expand an argument for an expansion function.
   The text starting at STR and ending at END is variable-expanded
   into a null-terminated string that is returned as the value.
   This is done without clobbering 'variable_buffer' or the current
   variable-expansion that is in progress.  */

char *
expand_argument (const char *str/*参数起始位置*/, const char *end/*参数结束位置*/)
{
  char *tmp, *alloc = NULL;
  char *r;

  if (str == end)
      /*参数为空*/
    return xstrdup ("");

  if (!end || *end == '\0')
      /*end为空，则str串全为此参数，展开此参数*/
    return allocated_variable_expand (str);

  /*申请内存*/
  if (end - str + 1 > 1000)
    tmp = alloc = xmalloc (end - str + 1);
  else
    tmp = alloca (end - str + 1);

  /*复制参数*/
  memcpy (tmp, str, end - str);
  tmp[end - str] = '\0';

  /*展开此参数*/
  r = allocated_variable_expand (tmp);

  free (alloc);

  return r;
}

/* Expand LINE for FILE.  Error messages refer to the file and line where
   FILE's commands were found.  Expansion uses FILE's variable set list.  */

char *
variable_expand_for_file (const char *line, struct file *file)
{
  char *result;
  struct variable_set_list *savev;
  const floc *savef;

  if (file == 0)
     /*file为空，仅需要展开line即可*/
    return variable_expand (line);

  /*全局变量保存(variable set list)*/
  savev = current_variable_set_list;

  /*取此文件对应的variables*/
  current_variable_set_list = file->variables;

  /*全局变量保存（reading file）*/
  savef = reading_file;
  if (file->cmds && file->cmds->fileinfo.filenm)
      /*使用file给定的fileinfo*/
    reading_file = &file->cmds->fileinfo;
  else
      /*非cmd情况，置为NULL*/
    reading_file = 0;

  /*全局变量都设置好了，开始展开*/
  result = variable_expand (line);

  /*还原全局变量*/
  current_variable_set_list = savev;
  reading_file = savef;

  return result;
}

/* Like allocated_variable_expand, but for += target-specific variables.
   First recursively construct the variable value from its appended parts in
   any upper variable sets.  Then expand the resulting value.  */

static char *
variable_append (const char *name, size_t length,
                 const struct variable_set_list *set, int local)
{
  const struct variable *v;
  char *buf = 0;
  int nextlocal;

  /* If there's nothing left to check, return the empty buffer.  */
  if (!set)
    return initialize_variable_output ();

  /* If this set is local and the next is not a parent, then next is local.  */
  nextlocal = local && set->next_is_parent == 0;

  /* Try to find the variable in this variable set.  */
  v = lookup_variable_in_set (name, length, set->set);

  /* If there isn't one, or this one is private, try the set above us.  */
  if (!v || (!local && v->private_var))
    return variable_append (name, length, set->next, nextlocal);

  /* If this variable type is append, first get any upper values.
     If not, initialize the buffer.  */
  if (v->append)
    buf = variable_append (name, length, set->next, nextlocal);
  else
    buf = initialize_variable_output ();

  /* Append this value to the buffer, and return it.
     If we already have a value, first add a space.  */
  if (buf > variable_buffer)
    buf = variable_buffer_output (buf, " ", 1);

  /* Either expand it or copy it, depending.  */
  if (! v->recursive)
    return variable_buffer_output (buf, v->value, strlen (v->value));

  buf = variable_expand_string (buf, v->value, strlen (v->value));
  return (buf + strlen (buf));
}


static char *
allocated_variable_append (const struct variable *v)
{
  char *val;

  /* Construct the appended variable value.  */

  char *obuf = variable_buffer;
  size_t olen = variable_buffer_length;

  variable_buffer = 0;

  val = variable_append (v->name, strlen (v->name),
                         current_variable_set_list, 1);
  variable_buffer_output (val, "", 1);
  val = variable_buffer;

  variable_buffer = obuf;
  variable_buffer_length = olen;

  return val;
}

/* Like variable_expand_for_file, but the returned string is malloc'd.
   This function is called a lot.  It wants to be efficient.  */

char *
allocated_variable_expand_for_file (const char *line, struct file *file)
{
  char *value;

  /*这里使用全局buffer，导致此函数不能嵌套，编码时考虑的有点多？
   * 为了解决这个问题，先用局部变量指向它，再将重新申请空间，就为了复用以前
   * 的函数，也是无奈
   * */
  char *obuf = variable_buffer;
  size_t olen = variable_buffer_length;

  variable_buffer = 0;

  /*利用file给定的变量信息，展开给定的line*/
  value = variable_expand_for_file (line, file);

  /*还原原来的全局buffer*/
  variable_buffer = obuf;
  variable_buffer_length = olen;

  return value;
}

/* Install a new variable_buffer context, returning the current one for
   safe-keeping.  */

void
install_variable_buffer (char **bufp, size_t *lenp)
{
  *bufp = variable_buffer;
  *lenp = variable_buffer_length;

  variable_buffer = 0;
  initialize_variable_output ();
}

/* Restore a previously-saved variable_buffer setting (free the current one).
 */

void
restore_variable_buffer (char *buf, size_t len)
{
  free (variable_buffer);

  variable_buffer = buf;
  variable_buffer_length = len;
}
