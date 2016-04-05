/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2015 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:                                                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_pcache.h"
#include "string.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <ctype.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <unistd.h>  
#include <fcntl.h>   
#include <pthread.h> 

#define BLOCK_SIZE 40768
#define BLOCK_NUM 5000
#define NODE_MAX_SIZE  100
#define KEY_NUM 1000
#define MAX_KEY_LENGTH 1000
#define DEFAULT_EXPIRE 3600
#define AT(i) (char *)(p+sizeof(head)+sizeof(node)*KEY_NUM+BLOCK_SIZE*(i))
struct node{
	long size;
	int is_free;
	int block[NODE_MAX_SIZE+1];
	char key[MAX_KEY_LENGTH];
	int key_len;
	time_t ttl;
};

typedef struct node node; 

struct head{
	unsigned int locate[BLOCK_NUM/sizeof(int)+1];
	int freeNodeNum;
	int freeBlockNum;
	int usedNodeNum;
	int usedBlockNum;
	node *p[KEY_NUM];
};
typedef struct head head;
head * global_head;
pthread_mutex_t* g_mutex;


/* True global resources - no need for thread safety here */
static char *p;
char temp_file[10];
/* If you declare any globals in php_pcache.h uncomment this:
ZEND_DECLARE_MODULE_GLOBALS(pcache)
*/

/* True global resources - no need for thread safety here */
static int le_pcache;

/* {{{ pcache_functions[]
 *
 * Every user visible function must have an entry in pcache_functions[].
 */
const zend_function_entry pcache_functions[] = {
	PHP_FE(confirm_pcache_compiled,	NULL)		/* For testing, remove later. */
	PHP_FE(pcache_add,	NULL)		/* For testing, remove later. */
	PHP_FE(pcache_get,	NULL)
	PHP_FE(pcache_del,	NULL)
	PHP_FE_END	/* Must be the last line in pcache_functions[] */
};
/* }}} */

/* {{{ pcache_module_entry
 */
zend_module_entry pcache_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"pcache",
	pcache_functions,
	PHP_MINIT(pcache),
	PHP_MSHUTDOWN(pcache),
	PHP_RINIT(pcache),		/* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(pcache),	/* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(pcache),
#if ZEND_MODULE_API_NO >= 20010901
	PHP_PCACHE_VERSION,
#endif
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_PCACHE
ZEND_GET_MODULE(pcache)
#endif

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("pcache.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_pcache_globals, pcache_globals)
    STD_PHP_INI_ENTRY("pcache.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_pcache_globals, pcache_globals)
PHP_INI_END()
*/
/* }}} */

/* {{{ php_pcache_init_globals
 */
/* Uncomment this function if you have INI entries
static void php_pcache_init_globals(zend_pcache_globals *pcache_globals)
{
	pcache_globals->global_value = 0;
	pcache_globals->global_string = NULL;
}
*/
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */

void makeShare()
{
	int flags=MAP_SHARED;
	sprintf(temp_file,"tmpXXXXXX");
	int fd = mkstemp(temp_file);
	if (ftruncate(fd, BLOCK_NUM*BLOCK_SIZE+sizeof(global_head)+sizeof(node)*KEY_NUM) < 0) 
        {
            close(fd);
            unlink(temp_file);
            printf("error 2\n");
        }
    unlink(temp_file);
	p = (char *)mmap(NULL, BLOCK_NUM*BLOCK_SIZE+sizeof(global_head)+sizeof(node)*KEY_NUM, PROT_READ | PROT_WRITE, flags, fd, 0);
	
	global_head=(head *)p;
	memset(global_head->locate, 0, sizeof(global_head->locate)*sizeof(int));
	int i;
	for (i = 0; i <KEY_NUM; ++i)
	{
		global_head->p[i]=(node *)((char *)p+sizeof(global_head)+i*sizeof(node));
		global_head->p[i]->is_free=1;
	}
	global_head->freeNodeNum=KEY_NUM;
	global_head->freeBlockNum=BLOCK_NUM;
	global_head->usedNodeNum=global_head->usedBlockNum=0;	
}
int findFreeBlock()
{
	int i,mod,index;
	for (i = 0; i < BLOCK_NUM; ++i)
	{
		mod=i%sizeof(int);
		index=i/sizeof(int);
		if(((1<<mod)&(global_head->locate[index]))==0)
			return i;
	}
	return -1;
}
int findFreeNode()
{
	int i;
	for (i = 0; i < KEY_NUM; ++i)
	{
		int check=(global_head->p[i])->is_free;
		if(check)
			return i;
		else if (global_head->p[i]->ttl<time(0))
		{
			removeKey(i);
			return i;
		}

	}
	return -1;
}
int setBlockUsed(int i)
{
	int mod,index;
	mod=i%sizeof(int);
	index=i/sizeof(int);
	global_head->locate[index]=global_head->locate[index]|(1<<mod);
}
int setBlockFree(int i)
{
	int mod,index;
	mod=i%sizeof(int);
	index=i/sizeof(int);
	global_head->locate[index]=global_head->locate[index]&(~(1<<mod));
}
int removeKey(int index)
{
	int * blockArr=global_head->p[index]->block;
	while(*blockArr!=-1)
	{
		setBlockFree(*blockArr);
		global_head->freeBlockNum++;
		global_head->usedBlockNum--;
		blockArr++;
	}
	global_head->freeNodeNum++;
	global_head->usedNodeNum--;
	global_head->p[index]->is_free=1;

}
int insertKey(char *key,int key_len,char *val,int val_len,int ttl)
{
	
	long len_t=val_len;
	int count=(val_len/BLOCK_SIZE)+1;
	if(global_head->freeBlockNum<count||global_head->freeNodeNum==0)
		return -1;
	int already=findKey(key,key_len);
	if(already!=-1)
		removeKey(already);
	int index=findFreeNode();
	if(index==-1)
		return -1;
	int i;
	int sum=0;
	for (i = 0; i < count; ++i)
	{
		int t=findFreeBlock();
		long ttt=len_t;
		if(t==-1)
			return -1;
		(global_head->p[index])->block[i]=t;
		if(ttt>BLOCK_SIZE)
			ttt=BLOCK_SIZE;
		memcpy(AT(t),val+sum,ttt);
		//long tt=snprintf(AT(t),ttt+1,"%s",val+sum);
		sum+=ttt;
		len_t-=ttt;
		setBlockUsed(t);
	}
	(global_head->p[index])->block[i]=-1;
	global_head->freeBlockNum-=count;
	global_head->usedBlockNum+=count;
	global_head->freeNodeNum--;
	global_head->p[index]->size=val_len;
	global_head->p[index]->is_free=0;
	global_head->p[index]->ttl=time(0)+ttl;

	snprintf(global_head->p[index]->key,key_len+1,"%s",key);
	global_head->p[index]->key_len=key_len;
	for (i = 0; i < count; ++i)
	{
		setBlockUsed((global_head->p[index])->block[i]);	
	}
	return 1;
}
int findKey(char *key,int key_len)
{
	int i;
	for(i=0;i<KEY_NUM;i++)
	{
		if(global_head->p[i]->is_free)
			continue;
		if(!memcmp(global_head->p[i]->key,key,key_len)&&(global_head->p[i]->key_len==key_len))
		{

			if(global_head->p[i]->ttl<time(0)){
				removeKey(i);
				return -1;
			}
			return i;
		}
	}
	return -1;
}

void init_mutex(void)  
{  
    int ret;  
    //g_mutex一定要是进程间可以共享的，否则无法达到进程间互斥  
    g_mutex=(pthread_mutex_t*)mmap(NULL, sizeof(pthread_mutex_t), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);  
    if( MAP_FAILED==g_mutex )  
    {  
        perror("mmap");  
        exit(1);  
    }  
      
    //设置attr的属性  
    pthread_mutexattr_t attr;  
    pthread_mutexattr_init(&attr);  
    //一定要设置为PTHREAD_PROCESS_SHARED  
    //具体可以参考http://blog.chinaunix.net/u/22935/showart_340408.html  
    ret=pthread_mutexattr_setpshared(&attr,PTHREAD_PROCESS_SHARED);  
    if( ret!=0 )  
    {  
        perror("init_mutex pthread_mutexattr_setpshared");  
        exit(1);  
    }  
    pthread_mutex_init(g_mutex, &attr);  
} 

PHP_MINIT_FUNCTION(pcache)
{
	/* If you have INI entries, uncomment these lines 
	REGISTER_INI_ENTRIES();
	*/
	makeShare();
	init_mutex();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(pcache)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(pcache)
{
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(pcache)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(pcache)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "pcache support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */


/* Remove the following function when you have successfully modified config.m4
   so that your module can be compiled into PHP, it exists only for testing
   purposes. */

/* Every user-visible function in PHP should document itself in the source */
/* {{{ proto string confirm_pcache_compiled(string arg)
   Return a string to confirm that the module is compiled in */
PHP_FUNCTION(confirm_pcache_compiled)
{
	char *arg = NULL;
	int arg_len, len;
	char *strg;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &arg, &arg_len) == FAILURE) {
		return;
	}

	len = spprintf(&strg, 0, "Congratulations! You have successfully modified ext/%.78s/config.m4. Module %.78s is now compiled into PHP.", "pcache", arg);
	RETURN_STRINGL(strg, len, 0);
}

PHP_FUNCTION(pcache_add)
{
 	if(pthread_mutex_lock(g_mutex)!=0 ) {
 		RETURN_FALSE;
 	}else{
		int len=0;
		char *arg = NULL;
		int arg_len;
		char *strg;
		char * name;
		int name_len;
		long ttl=DEFAULT_EXPIRE;
		zval *result;
		char *val;
		long val_len;
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|l", &name,&name_len,&val,&val_len,&ttl) == FAILURE) 
		{
			return;
		}
		int re=insertKey(name,name_len,val,val_len,ttl);
		pthread_mutex_unlock(g_mutex);  
		if (re==1){
			RETURN_TRUE;	
		}else{
			RETURN_FALSE;
		}
	}
}

PHP_FUNCTION(pcache_get)
{
	int len=0;
	char *arg = NULL;
	int arg_len;
	char *strg;
	char * name;
	int name_len;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s",&name,&name_len) == FAILURE) 
	{
			return;
	}
	int index=findKey(name,name_len);
	if (index==-1)
	{
		RETURN_NULL();
	}
	int * blockArr=global_head->p[index]->block;
	int length=global_head->p[index]->size;
	strg=emalloc(sizeof(char)*(length+100));
	int pos=0;
	while(*blockArr!=-1)
	{
		long tmp=length;
		if(length>BLOCK_SIZE)
			tmp=BLOCK_SIZE;
		memcpy(strg+pos,AT(*blockArr),tmp);
		length-=tmp;
		pos+=tmp;
		blockArr++;
	}
	RETURN_STRINGL(strg,pos,0);
}
PHP_FUNCTION(pcache_del)
{
	int len=0;
	char *arg = NULL;
	int arg_len;
	char *strg;
	char * name;
	int name_len;
	char *s=(char *)emalloc(100);
	int i=global_head->freeBlockNum;
	int j=global_head->freeNodeNum;
	int k=global_head->usedBlockNum;
	int l=global_head->usedNodeNum;
	//int n=sprintf(s,"free block num: %d free node num: %d",global_head->freeBlockNum,global_head->freeNodeNum);
	int n=sprintf(s,"free block num: %d free node num: %d used block num: %d used node num: %d",i,j,k,l);
	s[n]=0;
	RETURN_STRINGL(s,n,0);
}


/* }}} */
/* The previous line is meant for vim and emacs, so it can correctly fold and 
   unfold functions in source code. See the corresponding marks just before 
   function definition, where the functions purpose is also documented. Please 
   follow this convention for the convenience of others editing your code.
*/


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
