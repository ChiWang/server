/* Copyright (C) 2000 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "mysys_priv.h"
#include "mysys_err.h"
#include <m_ctype.h>
#include <m_string.h>
#include <my_dir.h>
#include <my_xml.h>


/*
  The code below implements this functionality:
  
    - Initializing charset related structures
    - Loading dynamic charsets
    - Searching for a proper CHARSET_INFO 
      using charset name, collation name or collation ID
    - Setting server default character set
*/

my_bool my_charset_same(CHARSET_INFO *cs1, CHARSET_INFO *cs2)
{
  return ((cs1 == cs2) || !strcmp(cs1->csname,cs2->csname));
}


static my_bool init_state_maps(CHARSET_INFO *cs)
{
  uint i;
  uchar *state_map;
  uchar *ident_map;

  if (!(cs->state_map= (uchar*) my_once_alloc(256, MYF(MY_WME))))
    return 1;
    
  if (!(cs->ident_map= (uchar*) my_once_alloc(256, MYF(MY_WME))))
    return 1;

  state_map= cs->state_map;
  ident_map= cs->ident_map;
  
  /* Fill state_map with states to get a faster parser */
  for (i=0; i < 256 ; i++)
  {
    if (my_isalpha(cs,i))
      state_map[i]=(uchar) MY_LEX_IDENT;
    else if (my_isdigit(cs,i))
      state_map[i]=(uchar) MY_LEX_NUMBER_IDENT;
#if defined(USE_MB) && defined(USE_MB_IDENT)
    else if (my_mbcharlen(cs, i)>1)
      state_map[i]=(uchar) MY_LEX_IDENT;
#endif
    else if (!my_isgraph(cs,i))
      state_map[i]=(uchar) MY_LEX_SKIP;
    else
      state_map[i]=(uchar) MY_LEX_CHAR;
  }
  state_map[(uchar)'_']=state_map[(uchar)'$']=(uchar) MY_LEX_IDENT;
  state_map[(uchar)'\'']=(uchar) MY_LEX_STRING;
  state_map[(uchar)'.']=(uchar) MY_LEX_REAL_OR_POINT;
  state_map[(uchar)'>']=state_map[(uchar)'=']=state_map[(uchar)'!']= (uchar) MY_LEX_CMP_OP;
  state_map[(uchar)'<']= (uchar) MY_LEX_LONG_CMP_OP;
  state_map[(uchar)'&']=state_map[(uchar)'|']=(uchar) MY_LEX_BOOL;
  state_map[(uchar)'#']=(uchar) MY_LEX_COMMENT;
  state_map[(uchar)';']=(uchar) MY_LEX_SEMICOLON;
  state_map[(uchar)':']=(uchar) MY_LEX_SET_VAR;
  state_map[0]=(uchar) MY_LEX_EOL;
  state_map[(uchar)'\\']= (uchar) MY_LEX_ESCAPE;
  state_map[(uchar)'/']= (uchar) MY_LEX_LONG_COMMENT;
  state_map[(uchar)'*']= (uchar) MY_LEX_END_LONG_COMMENT;
  state_map[(uchar)'@']= (uchar) MY_LEX_USER_END;
  state_map[(uchar) '`']= (uchar) MY_LEX_USER_VARIABLE_DELIMITER;
  state_map[(uchar)'"']= (uchar) MY_LEX_STRING_OR_DELIMITER;

  /*
    Create a second map to make it faster to find identifiers
  */
  for (i=0; i < 256 ; i++)
  {
    ident_map[i]= (uchar) (state_map[i] == MY_LEX_IDENT ||
			   state_map[i] == MY_LEX_NUMBER_IDENT);
  }

  /* Special handling of hex and binary strings */
  state_map[(uchar)'x']= state_map[(uchar)'X']= (uchar) MY_LEX_IDENT_OR_HEX;
  state_map[(uchar)'b']= state_map[(uchar)'b']= (uchar) MY_LEX_IDENT_OR_BIN;
  state_map[(uchar)'n']= state_map[(uchar)'N']= (uchar) MY_LEX_IDENT_OR_NCHAR;
  return 0;
}


static void simple_cs_init_functions(CHARSET_INFO *cs)
{
  if (cs->state & MY_CS_BINSORT)
    cs->coll= &my_collation_8bit_bin_handler;
  else
    cs->coll= &my_collation_8bit_simple_ci_handler;
  
  cs->cset= &my_charset_8bit_handler;
  cs->mbminlen= 1;
  cs->mbmaxlen= 1;
}



static int simple_cs_copy_data(CHARSET_INFO *to, CHARSET_INFO *from)
{
  to->number= from->number ? from->number : to->number;

  if (from->csname)
    if (!(to->csname= my_once_strdup(from->csname,MYF(MY_WME))))
      goto err;
  
  if (from->name)
    if (!(to->name= my_once_strdup(from->name,MYF(MY_WME))))
      goto err;
  
  if (from->comment)
    if (!(to->comment= my_once_strdup(from->comment,MYF(MY_WME))))
      goto err;
  
  if (from->ctype)
  {
    if (!(to->ctype= (uchar*) my_once_memdup((char*) from->ctype,
					     MY_CS_CTYPE_TABLE_SIZE,
					     MYF(MY_WME))))
      goto err;
    if (init_state_maps(to))
      goto err;
  }
  if (from->to_lower)
    if (!(to->to_lower= (uchar*) my_once_memdup((char*) from->to_lower,
						MY_CS_TO_LOWER_TABLE_SIZE,
						MYF(MY_WME))))
      goto err;

  if (from->to_upper)
    if (!(to->to_upper= (uchar*) my_once_memdup((char*) from->to_upper,
						MY_CS_TO_UPPER_TABLE_SIZE,
						MYF(MY_WME))))
      goto err;
  if (from->sort_order)
  {
    if (!(to->sort_order= (uchar*) my_once_memdup((char*) from->sort_order,
						  MY_CS_SORT_ORDER_TABLE_SIZE,
						  MYF(MY_WME))))
      goto err;

  }
  if (from->tab_to_uni)
  {
    uint sz= MY_CS_TO_UNI_TABLE_SIZE*sizeof(uint16);
    if (!(to->tab_to_uni= (uint16*)  my_once_memdup((char*)from->tab_to_uni,
						    sz, MYF(MY_WME))))
      goto err;
  }
  to->mbminlen= 1;
  to->mbmaxlen= 1;

  return 0;

err:
  return 1;
}


#ifdef HAVE_CHARSET_ucs2

typedef struct my_tailoring_st
{
  uint  number;
  const char *name;
  const char *tailoring;
} my_tailoring;

static my_tailoring tailoring[]=
{
  {
    0, "icelandic",
    /*
      Some sources treat LETTER A WITH DIARESIS (00E4,00C4)
      secondary greater than LETTER AE (00E6,00C6).
      http://www.evertype.com/alphabets/icelandic.pdf
      http://developer.mimer.com/collations/charts/icelandic.htm

      Other sources do not provide any special rules
      for LETTER A WITH DIARESIS:
      http://www.omniglot.com/writing/icelandic.htm
      http://en.wikipedia.org/wiki/Icelandic_alphabet
      http://oss.software.ibm.com/icu/charts/collation/is.html

      Let's go the first way.
    */
    "& A < \\u00E1 <<< \\u00C1 "
    "& D < \\u00F0 <<< \\u00D0 "
    "& E < \\u00E9 <<< \\u00C9 "
    "& I < \\u00ED <<< \\u00CD "
    "& O < \\u00F3 <<< \\u00D3 "
    "& U < \\u00FA <<< \\u00DA "
    "& Y < \\u00FD <<< \\u00DD "
    "& Z < \\u00FE <<< \\u00DE "
        "< \\u00E6 <<< \\u00C6 << \\u00E4 <<< \\u00C4 "
        "< \\u00F6 <<< \\u00D6 << \\u00F8 <<< \\u00D8 "
        "< \\u00E5 <<< \\u00C5 "
  },
  {
    1, "latvian",
    /*
      Some sources treat I and Y primary different.
      Other sources treat I and Y the same on primary level.
      We'll go the first way.
    */
    "& C < \\u010D <<< \\u010C "
    "& G < \\u0123 <<< \\u0122 "
    "& I < \\u0079 <<< \\u0059 "
    "& K < \\u0137 <<< \\u0136 "
    "& L < \\u013C <<< \\u013B "
    "& N < \\u0146 <<< \\u0145 "
    "& R < \\u0157 <<< \\u0156 "
    "& S < \\u0161 <<< \\u0160 "
    "& Z < \\u017E <<< \\u017D "
  },
  {
    2, "romanian",
    "& A < \\u0103 <<< \\u0102 < \\u00E2 <<< \\u00C2 "
    "& I < \\u00EE <<< \\u00CE "
    "& S < \\u0219 <<< \\u0218 << \\u015F <<< \\u015E "
    "& T < \\u021B <<< \\u021A << \\u0163 <<< \\u0162 "
  },
  {
    3, "slovenian",
    "& C < \\u010D <<< \\u010C "
    "& S < \\u0161 <<< \\u0160 "
    "& Z < \\u017E <<< \\u017D "
  },
  {
    4, "polish",
    "& A < \\u0105 <<< \\u0104 "
    "& C < \\u0107 <<< \\u0106 "
    "& E < \\u0119 <<< \\u0118 "
    "& L < \\u0142 <<< \\u0141 "
    "& N < \\u0144 <<< \\u0143 "
    "& O < \\u00F3 <<< \\u00D3 "
    "& S < \\u015B <<< \\u015A "
    "& Z < \\u017A <<< \\u017B "
  },
  {
    5, "estonian",
    "& S < \\u0161 <<< \\u0160 "
       " < \\u007A <<< \\u005A "
       " < \\u017E <<< \\u017D "
    "& W < \\u00F5 <<< \\u00D5 "
        "< \\u00E4 <<< \\u00C4 "
        "< \\u00F6 <<< \\u00D6 "
        "< \\u00FC <<< \\u00DC "
  },
  {
    6, "spanish",
    "& N < \\u00F1 <<< \\u00D1 "
  },
  {
    7, "swedish",
    /*
      Some sources treat V and W as similar on primary level.
      We'll treat V and W as different on primary level.
    */
    "& Y <<\\u00FC <<< \\u00DC "
    "& Z < \\u00E5 <<< \\u00C5 "
        "< \\u00E4 <<< \\u00C4 << \\u00E6 <<< \\u00C6 "
        "< \\u00F6 <<< \\u00D6 << \\u00F8 <<< \\u00D8 "
  },
  {
    8, "turkish",
    "& C < \\u00E7 <<< \\u00C7 "
    "& G < \\u011F <<< \\u011E "
    "& H < \\u0131 <<< \\u0049 "
    "& O < \\u00F6 <<< \\u00D6 "
    "& S < \\u015F <<< \\u015E "
    "& U < \\u00FC <<< \\u00DC "
  },
  {
    0, NULL, NULL
  }
};



static int ucs2_copy_data(CHARSET_INFO *to, CHARSET_INFO *from)
{
  
  to->number= from->number ? from->number : to->number;
  
  if (from->csname)
    if (!(to->csname= my_once_strdup(from->csname,MYF(MY_WME))))
      goto err;
  
  if (from->name)
    if (!(to->name= my_once_strdup(from->name,MYF(MY_WME))))
      goto err;
  
  if (from->comment)
    if (!(to->comment= my_once_strdup(from->comment,MYF(MY_WME))))
      goto err;
  
  if (from->tailoring)
    if (!(to->tailoring= my_once_strdup(from->tailoring,MYF(MY_WME))))
      goto err;
  
  to->strxfrm_multiply= my_charset_ucs2_general_uca.strxfrm_multiply;
  to->min_sort_char= my_charset_ucs2_general_uca.min_sort_char;
  to->max_sort_char= my_charset_ucs2_general_uca.max_sort_char;
  to->mbminlen= 2;
  to->mbmaxlen= 2;
  
  return 0;
  
err:
  return 1;
}
#endif


static my_bool simple_cs_is_full(CHARSET_INFO *cs)
{
  return ((cs->csname && cs->tab_to_uni && cs->ctype && cs->to_upper &&
	   cs->to_lower) &&
	  (cs->number && cs->name &&
	  (cs->sort_order || (cs->state & MY_CS_BINSORT) )));
}


static int add_collation(CHARSET_INFO *cs)
{
  if (cs->name && (cs->number || (cs->number=get_collation_number(cs->name))))
  {
    if (!all_charsets[cs->number])
    {
      if (!(all_charsets[cs->number]=
         (CHARSET_INFO*) my_once_alloc(sizeof(CHARSET_INFO),MYF(0))))
        return MY_XML_ERROR;
      bzero((void*)all_charsets[cs->number],sizeof(CHARSET_INFO));
    }
    
    if (cs->primary_number == cs->number)
      cs->state |= MY_CS_PRIMARY;
      
    if (cs->binary_number == cs->number)
      cs->state |= MY_CS_BINSORT;
    
    all_charsets[cs->number]->state|= cs->state;
    
    if (!(all_charsets[cs->number]->state & MY_CS_COMPILED))
    {
      if (!strcmp(cs->csname,"ucs2") )
      {
#ifdef HAVE_CHARSET_ucs2
        CHARSET_INFO *new= all_charsets[cs->number];
        new->cset= my_charset_ucs2_general_uca.cset;
        new->coll= my_charset_ucs2_general_uca.coll;
        if (ucs2_copy_data(new, cs))
          return MY_XML_ERROR;
        new->state |= MY_CS_AVAILABLE | MY_CS_LOADED;
#endif        
      }
      else
      {
        simple_cs_init_functions(all_charsets[cs->number]);
        if (simple_cs_copy_data(all_charsets[cs->number],cs))
	  return MY_XML_ERROR;
        if (simple_cs_is_full(all_charsets[cs->number]))
        {
          all_charsets[cs->number]->state |= MY_CS_LOADED;
        }
        all_charsets[cs->number]->state|= MY_CS_AVAILABLE;
      }
    }
    else
    {
      /*
        We need the below to make get_charset_name()
        and get_charset_number() working even if a
        character set has not been really incompiled.
        The above functions are used for example
        in error message compiler extra/comp_err.c.
        If a character set was compiled, this information
        will get lost and overwritten in add_compiled_collation().
      */
      CHARSET_INFO *dst= all_charsets[cs->number];
      dst->number= cs->number;
      if (cs->comment)
	if (!(dst->comment= my_once_strdup(cs->comment,MYF(MY_WME))))
	  return MY_XML_ERROR;
      if (cs->csname)
        if (!(dst->csname= my_once_strdup(cs->csname,MYF(MY_WME))))
	  return MY_XML_ERROR;
      if (cs->name)
	if (!(dst->name= my_once_strdup(cs->name,MYF(MY_WME))))
	  return MY_XML_ERROR;
    }
    cs->number= 0;
    cs->primary_number= 0;
    cs->binary_number= 0;
    cs->name= NULL;
    cs->state= 0;
    cs->sort_order= NULL;
    cs->state= 0;
  }
  return MY_XML_OK;
}

#ifdef HAVE_CHARSET_ucs2
static my_bool init_uca_charsets()
{
  my_tailoring *t;
  CHARSET_INFO cs= my_charset_ucs2_general_uca;
  char name[64];
  
  cs.state= MY_CS_STRNXFRM|MY_CS_UNICODE;
  for (t= tailoring; t->tailoring; t++)
  {
    cs.number= 128 + t->number;
    cs.tailoring= t->tailoring;
    cs.name= name;
    sprintf(name, "ucs2_%s_ci", t->name);
    add_collation(&cs);
  }
  return 0;
}
#endif

#define MY_MAX_ALLOWED_BUF 1024*1024
#define MY_CHARSET_INDEX "Index.xml"

const char *charsets_dir= NULL;
static int charset_initialized=0;


static my_bool my_read_charset_file(const char *filename, myf myflags)
{
  char *buf;
  int  fd;
  uint len;
  MY_STAT stat_info;
  
  if (!my_stat(filename, &stat_info, MYF(myflags)) ||
       ((len= (uint)stat_info.st_size) > MY_MAX_ALLOWED_BUF) ||
       !(buf= (char *)my_malloc(len,myflags)))
    return TRUE;
  
  if ((fd=my_open(filename,O_RDONLY,myflags)) < 0)
  {
    my_free(buf,myflags);
    return TRUE;
  }
  len=read(fd,buf,len);
  my_close(fd,myflags);
  
  if (my_parse_charset_xml(buf,len,add_collation))
  {
#ifdef NOT_YET
    printf("ERROR at line %d pos %d '%s'\n",
	   my_xml_error_lineno(&p)+1,
	   my_xml_error_pos(&p),
	   my_xml_error_string(&p));
#endif
  }
  
  my_free(buf, myflags);  
  return FALSE;
}


char *get_charsets_dir(char *buf)
{
  const char *sharedir= SHAREDIR;
  char *res;
  DBUG_ENTER("get_charsets_dir");

  if (charsets_dir != NULL)
    strmake(buf, charsets_dir, FN_REFLEN-1);
  else
  {
    if (test_if_hard_path(sharedir) ||
	is_prefix(sharedir, DEFAULT_CHARSET_HOME))
      strxmov(buf, sharedir, "/", CHARSET_DIR, NullS);
    else
      strxmov(buf, DEFAULT_CHARSET_HOME, "/", sharedir, "/", CHARSET_DIR,
	      NullS);
  }
  res= convert_dirname(buf,buf,NullS);
  DBUG_PRINT("info",("charsets dir: '%s'", buf));
  DBUG_RETURN(res);
}

CHARSET_INFO *all_charsets[256];
CHARSET_INFO *default_charset_info = &my_charset_latin1;

void add_compiled_collation(CHARSET_INFO *cs)
{
  all_charsets[cs->number]= cs;
  cs->state|= MY_CS_AVAILABLE;
}

static void *cs_alloc(uint size)
{
  return my_once_alloc(size, MYF(MY_WME));
}


#ifdef __NETWARE__
my_bool STDCALL init_available_charsets(myf myflags)
#else
static my_bool init_available_charsets(myf myflags)
#endif
{
  char fname[FN_REFLEN];
  my_bool error=FALSE;
  /*
    We have to use charset_initialized to not lock on THR_LOCK_charset
    inside get_internal_charset...
  */
  if (!charset_initialized)
  {
    CHARSET_INFO **cs;
    /*
      To make things thread safe we are not allowing other threads to interfere
      while we may changing the cs_info_table
    */
    pthread_mutex_lock(&THR_LOCK_charset);

    bzero(&all_charsets,sizeof(all_charsets));
    init_compiled_charsets(myflags);
#ifdef HAVE_CHARSET_ucs2
    init_uca_charsets();
#endif
    
    /* Copy compiled charsets */
    for (cs=all_charsets;
	 cs < all_charsets+array_elements(all_charsets)-1 ;
	 cs++)
    {
      if (*cs)
      {
        if (cs[0]->ctype)
          if (init_state_maps(*cs))
            *cs= NULL;
      }
    }
    
    strmov(get_charsets_dir(fname), MY_CHARSET_INDEX);
    error= my_read_charset_file(fname,myflags);
    charset_initialized=1;
    pthread_mutex_unlock(&THR_LOCK_charset);
  }
  return error;
}


void free_charsets(void)
{
  charset_initialized=0;
}


uint get_collation_number(const char *name)
{
  CHARSET_INFO **cs;
  init_available_charsets(MYF(0));
  
  for (cs= all_charsets;
       cs < all_charsets+array_elements(all_charsets)-1 ;
       cs++)
  {
    if ( cs[0] && cs[0]->name && 
         !my_strcasecmp(&my_charset_latin1, cs[0]->name, name))
      return cs[0]->number;
  }  
  return 0;   /* this mimics find_type() */
}


uint get_charset_number(const char *charset_name, uint cs_flags)
{
  CHARSET_INFO **cs;
  init_available_charsets(MYF(0));
  
  for (cs= all_charsets;
       cs < all_charsets+array_elements(all_charsets)-1 ;
       cs++)
  {
    if ( cs[0] && cs[0]->csname && (cs[0]->state & cs_flags) &&
         !my_strcasecmp(&my_charset_latin1, cs[0]->csname, charset_name))
      return cs[0]->number;
  }  
  return 0;
}


const char *get_charset_name(uint charset_number)
{
  CHARSET_INFO *cs;
  init_available_charsets(MYF(0));

  cs=all_charsets[charset_number];
  if (cs && (cs->number == charset_number) && cs->name )
    return (char*) cs->name;
  
  return (char*) "?";   /* this mimics find_type() */
}


static CHARSET_INFO *get_internal_charset(uint cs_number, myf flags)
{
  char  buf[FN_REFLEN];
  CHARSET_INFO *cs;
  /*
    To make things thread safe we are not allowing other threads to interfere
    while we may changing the cs_info_table
  */
  pthread_mutex_lock(&THR_LOCK_charset);
  if ((cs= all_charsets[cs_number]))
  {
    if (!(cs->state & MY_CS_COMPILED) && !(cs->state & MY_CS_LOADED))
    {
      strxmov(get_charsets_dir(buf), cs->csname, ".xml", NullS);
      my_read_charset_file(buf,flags);
    }
    cs= (cs->state & MY_CS_AVAILABLE) ? cs : NULL;
  }
  pthread_mutex_unlock(&THR_LOCK_charset);
  if (cs && !(cs->state & MY_CS_READY))
  {
    if ((cs->cset->init && cs->cset->init(cs, cs_alloc)) ||
        (cs->coll->init && cs->coll->init(cs, cs_alloc)))
      cs= NULL;
    else
      cs->state|= MY_CS_READY;
  }
  return cs;
}


CHARSET_INFO *get_charset(uint cs_number, myf flags)
{
  CHARSET_INFO *cs;
  if (cs_number == default_charset_info->number)
    return default_charset_info;

  (void) init_available_charsets(MYF(0));	/* If it isn't initialized */
  
  if (!cs_number || cs_number >= array_elements(all_charsets)-1)
    return NULL;
  
  cs=get_internal_charset(cs_number, flags);

  if (!cs && (flags & MY_WME))
  {
    char index_file[FN_REFLEN], cs_string[23];
    strmov(get_charsets_dir(index_file),MY_CHARSET_INDEX);
    cs_string[0]='#';
    int10_to_str(cs_number, cs_string+1, 10);
    my_error(EE_UNKNOWN_CHARSET, MYF(ME_BELL), cs_string, index_file);
  }
  return cs;
}

CHARSET_INFO *get_charset_by_name(const char *cs_name, myf flags)
{
  uint cs_number;
  CHARSET_INFO *cs;
  (void) init_available_charsets(MYF(0));	/* If it isn't initialized */

  cs_number=get_collation_number(cs_name);
  cs= cs_number ? get_internal_charset(cs_number,flags) : NULL;

  if (!cs && (flags & MY_WME))
  {
    char index_file[FN_REFLEN];
    strmov(get_charsets_dir(index_file),MY_CHARSET_INDEX);
    my_error(EE_UNKNOWN_CHARSET, MYF(ME_BELL), cs_name, index_file);
  }

  return cs;
}


CHARSET_INFO *get_charset_by_csname(const char *cs_name,
				    uint cs_flags,
				    myf flags)
{
  uint cs_number;
  CHARSET_INFO *cs;
  DBUG_ENTER("get_charset_by_csname");
  DBUG_PRINT("enter",("name: '%s'", cs_name));

  (void) init_available_charsets(MYF(0));	/* If it isn't initialized */
  
  cs_number= get_charset_number(cs_name, cs_flags);
  cs= cs_number ? get_internal_charset(cs_number, flags) : NULL;
  
  if (!cs && (flags & MY_WME))
  {
    char index_file[FN_REFLEN];
    strmov(get_charsets_dir(index_file),MY_CHARSET_INDEX);
    my_error(EE_UNKNOWN_CHARSET, MYF(ME_BELL), cs_name, index_file);
  }

  DBUG_RETURN(cs);
}


ulong escape_string_for_mysql(CHARSET_INFO *charset_info, char *to,
                              const char *from, ulong length)
{
  const char *to_start= to;
  const char *end;
#ifdef USE_MB
  my_bool use_mb_flag= use_mb(charset_info);
#endif
  for (end= from + length; from != end; from++)
  {
#ifdef USE_MB
    int l;
    if (use_mb_flag && (l= my_ismbchar(charset_info, from, end)))
    {
      while (l--)
	*to++= *from++;
      from--;
      continue;
    }
#endif
    switch (*from) {
    case 0:				/* Must be escaped for 'mysql' */
      *to++= '\\';
      *to++= '0';
      break;
    case '\n':				/* Must be escaped for logs */
      *to++= '\\';
      *to++= 'n';
      break;
    case '\r':
      *to++= '\\';
      *to++= 'r';
      break;
    case '\\':
      *to++= '\\';
      *to++= '\\';
      break;
    case '\'':
      *to++= '\\';
      *to++= '\'';
      break;
    case '"':				/* Better safe than sorry */
      *to++= '\\';
      *to++= '"';
      break;
    case '\032':			/* This gives problems on Win32 */
      *to++= '\\';
      *to++= 'Z';
      break;
    default:
      *to++= *from;
    }
  }
  *to= 0;
  return (ulong) (to - to_start);
}
