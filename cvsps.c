#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <search.h>
#include <time.h>
#include <common/hash.h>
#include <common/list.h>
#include <common/text_util.h>
#include <common/debug.h>

#define CVS_LOG_MAX 8192

enum
{
    NEED_FILE,
    NEED_START_LOG,
    NEED_REVISION,
    NEED_DATE_AND_AUTHOR,
    NEED_EOM
};

typedef struct _CvsFile
{
    char filename[PATH_MAX];
    struct list_head patch_sets;
} CvsFile;

typedef struct _PatchSet
{
    char id[16];
    time_t date;
    char author[64];
    char descr[CVS_LOG_MAX];
    struct list_head members;
} PatchSet;

typedef struct _PatchSetMember
{
    char pre_rev[16];
    char post_rev[16];
    PatchSet * cp;
    CvsFile * file;
    struct list_head file_link;
    struct list_head patch_set_link;
} PatchSetMember;

static int ps_counter;
static struct hash_table * file_hash;
static void * ps_tree;


static CvsFile * parse_file(const char *);
static PatchSetMember * parse_revision(const char *);
static PatchSet * get_patch_set(const char *, const char *, const char *);
static void assign_pre_revision(PatchSetMember *, PatchSetMember *);
static void show_ps_tree_node(const void *, const VISIT, const int);
static int compare_patch_sets(const void *, const void *);
static void convert_date(time_t *, const char *);
static int is_revision_metadata(const char *);

int main()
{
    FILE * cvsfp;
    char buff[BUFSIZ];
    int state = NEED_FILE;
    CvsFile * file = NULL;
    PatchSetMember * psm = NULL, * last_psm = NULL;
    char datebuff[20];
    char authbuff[64];
    char logbuff[CVS_LOG_MAX];
    int have_log = 0;
    
    //chdir("../pricing_engine");

    file_hash = create_hash_table(1023);
    ps_tree = NULL;

    cvsfp = popen("cvs log", "r");

    if (!cvsfp)
    {
	perror("can't open cvs pipe\n");
	exit(1);
    }
    
    while(fgets(buff, BUFSIZ, cvsfp))
    {
	//debug(DEBUG_STATUS, "state: %d read line:%s", state, buff);

	switch(state)
	{
	case NEED_FILE:
	    if (strncmp(buff, "RCS file", 8) == 0)
	    {
		file = parse_file(buff);
		state++;
	    }
	    break;
	case NEED_START_LOG:
	    if (strncmp(buff, "--------", 8) == 0)
		state++;
	    break;
	case NEED_REVISION:
	    if (strncmp(buff, "revision", 8) == 0)
	    {
		psm = parse_revision(buff);
		psm->file = file;

		/* in the simple case, we are copying psm->post_rev to last_psm->pre_rev
		 * since generally speaking the log is reverse chronological.
		 * This breaks down slightly when branches are introduced 
		 */
		assign_pre_revision(last_psm, psm);
		last_psm = psm;
		list_add(&psm->file_link, file->patch_sets.prev);
		state++;
	    }
	    break;
	case NEED_DATE_AND_AUTHOR:
	    if (strncmp(buff, "date:", 5) == 0)
	    {
		char * p;
		strncpy(datebuff, buff + 6, 19);
		datebuff[19] = 0;
		p = strstr(buff, "author: ");
		if (p)
		{
		    char * op;
		    p += 8;
		    op = strchr(p, ';');
		    if (op)
		    {
			strncpy(authbuff, p, op - p);
			authbuff[op - p] = 0;
		    }
		}
		
		state++;
	    }
	    break;
	case NEED_EOM:
	    if (strncmp(buff, "--------", 8) == 0)
	    {
		psm->cp = get_patch_set(datebuff, logbuff, authbuff);
		list_add(&psm->patch_set_link, &psm->cp->members);
		datebuff[0] = 0;
		logbuff[0] = 0;
		psm = NULL;
		state = NEED_REVISION;
		have_log = 0;
	    }
	    else if (strncmp(buff, "========", 8) == 0)
	    {
		psm->cp = get_patch_set(datebuff, logbuff, authbuff);
		list_add(&psm->patch_set_link, &psm->cp->members);
		datebuff[0] = 0;
		logbuff[0] = 0;
		assign_pre_revision(last_psm, NULL);
		psm = NULL;
		last_psm = NULL;
		file = NULL;
		state = NEED_FILE;
		have_log = 0;
	    }
	    else
	    {
		//FIXME: no silent buffer overflow
		/* other "blahblah: information;" messages can 
		 * follow the stuff we pay attention to
		 */
		if (have_log || !is_revision_metadata(buff))
		{
		    have_log = 1;
		    strcat(logbuff, buff);
		}
		else 
		{
		    debug(DEBUG_STATUS, "ignoring unneeded info %s", buff);
		}
	    }

	    break;
	}
    }

    pclose(cvsfp);
    twalk(ps_tree, show_ps_tree_node);
    exit(0);
}

static CvsFile * parse_file(const char * buff)
{
    CvsFile * retval;

    retval = (CvsFile*)get_hash_object(file_hash, buff + 10);

    if (!retval)
    {
	if ((retval = (CvsFile*)malloc(sizeof(*retval))))
	{
	    strcpy(retval->filename, buff + 10);
	    chop(retval->filename);
	    INIT_LIST_HEAD(&retval->patch_sets);
	    put_hash_object(file_hash, retval->filename, retval);
	}
    }

    debug(DEBUG_STATUS, "new file: %s", retval->filename);

    return retval;
}

static PatchSetMember * parse_revision(const char * buff)
{
    PatchSetMember * retval = (PatchSetMember*)malloc(sizeof(*retval));

    //FIXME: what about pre_rev?
    strcpy(retval->pre_rev, "UNKNOWN");
    strcpy(retval->post_rev, buff + 9);
    chop(retval->post_rev);
    retval->cp = NULL;

    debug(DEBUG_STATUS, "new rev: %s", retval->post_rev);

    return retval;
}

static PatchSet * get_patch_set(const char * dte, const char * log, const char * author)
{
    PatchSet * retval = NULL, **find = NULL;
    
    if (!(retval = (PatchSet*)malloc(sizeof(*retval))))
    {
	debug(DEBUG_SYSERROR, "malloc failed for PatchSet");
	return NULL;
    }

    sprintf(retval->id, "%d", ps_counter);
    convert_date(&retval->date, dte);
    strcpy(retval->author, author);
    strcpy(retval->descr, log);
    INIT_LIST_HEAD(&retval->members);

    find = (PatchSet**)tsearch(retval, &ps_tree, compare_patch_sets);

    if (*find != retval)
    {
	debug(DEBUG_STATUS, "found existing patch set");
	free(retval);
	retval = *find;
    }
    else
    {
	debug(DEBUG_STATUS, "new patch set!");
	debug(DEBUG_STATUS, "%s %s %s", retval->author, retval->descr, dte);
	ps_counter++;
    }

    return retval;
}

static int get_branch(char * buff, const char * rev)
{
    char * p;
    strcpy(buff, rev);
    p = strrchr(buff, '.');
    if (!p)
	return 0;
    *p = 0;
    return 1;
}

static void assign_pre_revision(PatchSetMember * last_psm, PatchSetMember * psm)
{
    char pre[16], post[16];

    if (!last_psm)
	return;
    
    if (!psm)
    {
	strcpy(last_psm->pre_rev, "INITIAL");
	return;
    }

    /* are the two revisions on the same branch? */
    if (!get_branch(pre, psm->post_rev))
	return;

    if (!get_branch(post, last_psm->post_rev))
	return;

    if (strcmp(pre, post) == 0)
    {
	strcpy(last_psm->pre_rev, psm->post_rev);
	return;
    }
    
    /* branches don't match. psm must be head of branch,
     * so last_psm is first rev. on branch. or first
     * revision overall.  if former, derive predecessor.  
     * use get_branch to chop another rev. off of string.
     */
    if (!get_branch(pre, post))
    {
	strcpy(last_psm->pre_rev, "INITIAL");
	return;
    }
    
    strcpy(last_psm->pre_rev, pre);
}

static void show_ps_tree_node(const void * nodep, const VISIT which, const int depth)
{
    PatchSet * ps;
    struct list_head * next;
    struct tm * tm;

    switch(which)
    {
    case postorder:
    case leaf:
	ps = *(PatchSet**)nodep;
	next = ps->members.next;
	tm = localtime(&ps->date);

	printf("---------------------\n");
	printf("PatchSet %s\n", ps->id);
	printf("Date: %d/%02d/%02d %02d:%02d:%02d\n", 
	       1900 + tm->tm_year, tm->tm_mon, tm->tm_mday, 
	       tm->tm_hour, tm->tm_min, tm->tm_sec);
	printf("Author: %s\n", ps->author);
	printf("Log:\n%s", ps->descr);
	printf("Members: \n");

	while (next != &ps->members)
	{
	    PatchSetMember * psm = list_entry(next, PatchSetMember, patch_set_link);
	    printf("\t%s:%s->%s\n", psm->file->filename, psm->pre_rev, psm->post_rev);
	    next = next->next;
	}
	
	printf("\n");
    default:
	break;
    }
}

static int compare_patch_sets(const void * v_ps1, const void * v_ps2)
{
    const PatchSet * ps1 = (const PatchSet *)v_ps1;
    const PatchSet * ps2 = (const PatchSet *)v_ps2;
    int ret;
    long diff;

    ret = strcmp(ps1->author, ps2->author);

    if (ret)
	return ret;

    ret = strcmp(ps1->descr, ps2->descr);

    if (ret)
	return ret;
    
    diff = ps1->date - ps2->date;

    if (labs(diff) < 300)
	return 0;

    return (diff < 0) ? -1 : 1;
}

static void convert_date(time_t * t, const char * dte)
{
    struct tm tm;
    int ret;
    
    memset(&tm, 0, sizeof(tm));
    ret = sscanf(dte, "%d/%d/%d %d:%d:%d", 
	   &tm.tm_year, &tm.tm_mon, &tm.tm_mday, 
	   &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    
    tm.tm_year -= 1900;
    *t = mktime(&tm);
}

static int is_revision_metadata(const char * buff)
{
    char * p1, *p2;
    int len;

    if (!(p1 = strchr(buff, ':')))
	return 0;

    p2 = strchr(buff, ' ');
    
    if (p2 && p2 < p1)
	return 0;

    len = strlen(buff);

    /* lines have LF at end */
    if (len > 1 && buff[len - 2] == ';')
	return 1;

    return 0;
}
