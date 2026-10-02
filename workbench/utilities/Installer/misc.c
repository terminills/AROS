/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
*/

/* misc.c -- here are all miscellaneous functions for global use */

#include "Installer.h"
#include "cleanup.h"
#include "execute.h"
#include "variables.h"

/* External variables */
extern InstallerPrefs preferences;


/*
 * Break string into array of strings at LINEFEEDs
 * **outarr must be obtained via malloc!
 */
int strtostrs(char * in, char ***outarr)
{
int i = 0, j = 0, k = 0;
char **out = *outarr;

    while (*in)
    {
        i++;
        /* allocate space for next string */
        out = realloc(out, (i+1) * sizeof(char *));
        outofmem(out);
        for ( j = 0 ; in[j] && in[j]!=LINEFEED ; j++ );
        out[i-1] = malloc((j+1) * sizeof(char));
        outofmem(out[i-1]);
        for ( k = 0 ; k < j ; k++ )
        {
            /* save char to string */
            out[i-1][k] = *in;
            in++;
        }
        /* NULL-terminate string */
        out[i-1][j] = 0;
        if (*in)
            in++;
    }
    /* NULL-terminate array */
    out[i] = NULL;
    *outarr = out;

return i;
}

/*
 * Collate array of strings with glueing LINEFEEDs
 */
char *collatestrings(int n, char ** instrs)
{
char *retval;
int len = 0, i, j, k;

    for ( i = 0 ; i < n ; i++ )
    {
        /* Add length of line to total length of text,
           plus additional LINEFEED (NULL for last item) */
        len += strlen(instrs[i]) + 1;
    }
    retval = malloc(sizeof(char) * len);
    if (retval != NULL)
    {
        j = 0;
        for ( i = 0 ; i < n ; i++ )
        {
            k = 0;
            for ( len = strlen(instrs[i]) ; len > 0 ; len-- )
            {
                retval[j] = instrs[i][k];
                j++;
                k++;
            }
            retval[j] = LINEFEED;
            j++;
        }
        retval[j-1] = 0;
    }

return retval;
}

/*
 * Add surrounding quotes to string
 * Creates a copy of input string
 */
char *addquotes(char * string)
{
char *retval;
int c;

    /* Add surrounding quotes */
    c = (string == NULL) ? 0 : strlen(string);
    retval = malloc(c+3);
    outofmem(retval);
    retval[0] = DQUOTE;
    /* NULL is what collect_strings() returns for no strings at all -
     * "(welcome)" on its own - and that is the empty string, "" */
    if (c > 0)
    {
        strcpy(retval+1, string);
    }
    retval[c+1] = DQUOTE;
    retval[c+2] = 0;

return retval;
}

/*
 * free() array of strings (eg. allocated by strtostrs())
 */
void freestrlist(STRPTR *array)
{
int i=0;

    while (array[i])
    {
        free(array[i]);
        i++;
    }
    free(array);
}



/*
 * The installed-files registry: one entry per application, named after
 * APPNAME, in the drawer ENV:Installer/Registry names (default
 * SYS:Prefs/Installer/Registry -- a live CD points it at RAM:). Each line is
 * one thing the script created:
 *   F <file>    D <directory>    A <assign>    S <User-Startup section>
 *   T <icon whose tooltypes were changed>
 * so the application can be removed again. The entry is opened on the first
 * thing really created: a pretend run, or a run aborted before it did
 * anything, leaves the previous entry alone.
 */
static void registry_open(void)
{
char dir[256], *name, *p;
STRPTR path;
LONG len;
BPTR lock;

    if (GetVar("Installer/Registry", dir, sizeof(dir), 0) <= 0)
    {
        strcpy(dir, "SYS:Prefs/Installer/Registry");
    }
    /* create the drawer and any missing parents */
    for (p = dir; ; p++)
    {
        if (*p == '/' || *p == 0)
        {
            char c = *p;
            *p = 0;
            if (p > dir && p[-1] != ':' && (lock = Lock(dir, SHARED_LOCK)) == BNULL)
            {
                lock = CreateDir(dir);
            }
            if (lock != BNULL)
            {
                UnLock(lock);
            }
            *p = c;
            if (c == 0)
            {
                break;
            }
        }
    }
    p = get_var_arg("@app-name");
    name = strdup(p != NULL && *p ? p : "Unnamed");
    outofmem(name);
    for (p = name; *p; p++)
    {
        if (*p == '/' || *p == ':' || *p == '"' || *p == '*' || *p == '?' || *p == '#')
        {
            *p = '_';
        }
    }
    len = strlen(dir) + strlen(name) + 2;
    path = malloc(len);
    outofmem(path);
    strcpy(path, dir);
    AddPart(path, name, len);
    preferences.registrystream = Open(path, MODE_NEWFILE);
    free(path);
    free(name);
}

/*
 * Record one thing the script created in this application's registry entry.
 * F/D/T paths are canonicalised with NameFromLock() so removing the
 * application later does not depend on the assigns the script used. Callers
 * only log what really happened, never in pretend mode.
 */
void registry_log(char kind, const char *path)
{
BPTR lock;
char *full = NULL;
char prefix[2];

    if (preferences.noregistry || preferences.pretend || path == NULL)
    {
        return;
    }
    if (preferences.registrystream == BNULL)
    {
        registry_open();
        if (preferences.registrystream == BNULL)
        {
            /* read-only system and no ENV:Installer/Registry: carry on */
            preferences.noregistry = 1;
            return;
        }
    }
    if (kind == 'F' || kind == 'D' || kind == 'T')
    {
        lock = Lock((STRPTR)path, SHARED_LOCK);
        if (lock != BNULL)
        {
            full = DynNameFromLock(lock);
            UnLock(lock);
        }
    }
    prefix[0] = kind;
    prefix[1] = SPACE;
    Write(preferences.registrystream, prefix, 2);
    Write(preferences.registrystream, full ? full : (char *)path, strlen(full ? full : path));
    Write(preferences.registrystream, "\n", 1);
    free(full);
}
