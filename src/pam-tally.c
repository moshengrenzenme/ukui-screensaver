/*
 * Copyright (C) 2018 Tianjin KYLIN Information Technology Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
**/

#include "pam-tally.h"

#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>

//#include "gs-debug.h"

char shm_tally_real[128];

#define FILE_MODE (S_IRUSR | S_IWUSR)

#define CONFIG_FILE "/usr/share/lightdm/lightdm.conf.d/96-kylin-setting.conf"



static
int get_is_open_other_authentication()
{
    char buf[128];
    FILE *config_file;

    if( (config_file = fopen(CONFIG_FILE, "r")) == NULL)
    {
        //gs_debug("open %s failed", CONFIG_FILE);
        return 0;
    }

    int open_other_authentication = 0;
    while(fgets(buf, sizeof(buf), config_file)) {
        if(strlen(buf) == 0 || buf[0] == '#')
        {
            memset(buf, sizeof(buf), 0);
            continue;
        }
        if(buf[strlen(buf)-1] == '\n')
            buf[strlen(buf)-1] = '\0';

        char *p = strchr(buf, '=');
        if(!p)
            continue;
        *p = '\0';

        size_t len = strlen(buf);
        if(len == 0)
            continue;
        //去掉=之前的空格
        while(len--)
            if(buf[len] == ' ' || buf[len] == '\t')
                buf[len] = '\0';
        if(strcmp(buf, "open-other-authentication") != 0)
            continue;

        p++;
        len = strlen(p);
        if(len == 0)
            break;
        //去掉等号之后的空格
        while(*p == ' ' || *p == '\t')
        {
            p++;
            len--;
        }
        //去掉尾部空格
        while(len--)
            if(*(p+len) == ' ' || *(p+len) == '\t')
                *(p+len) = '\0';

        if(*p == '0')
            break;
        if(*p == '1')
        {
            open_other_authentication = 1;
            break;
        }
    }
    fclose(config_file);
    //gs_debug("--------------------------%d", open_other_authentication);
    return open_other_authentication;
}

static
int get_pam_tally(int *deny, int *unlock_time)
{
    char buf[128];
    FILE *auth_file;

    if( (auth_file = fopen("/etc/pam.d/common-auth", "r")) == NULL)
        return -1;

    while(fgets(buf, sizeof(buf), auth_file)) {
        if(strlen(buf) == 0 || buf[0] == '#')
            continue;
        if(!strstr(buf, "deny"))
            continue;

        char *ptr = strtok(buf, " \t");
        while(ptr) {
            if(strncmp(ptr, "deny=", 5)==0){
                sscanf(ptr, "deny=%d", deny);
                //gs_debug("-------------------- deny=%d", *deny);
            }
            if(strncmp(ptr, "unlock_time=", 12)==0){
                sscanf(ptr, "unlock_time=%d", unlock_time);
                //gs_debug("-------------------- unlock_time=%d", *unlock_time);
            }
            ptr = strtok(NULL, " \t");
        }
        return 1;
    }
    return 0;
}

static
void set_shm_tally_real()
{
    sprintf(shm_tally_real, "%s_%d", SHM_TALLY, getuid());
}

int pam_tally_init()
{
    int fd;
    int deny = 0, unlock_time = 0;
    pam_tally *tally_ptr;

    set_shm_tally_real();

    printf("shm path =========== : %s\n", shm_tally_real);

    shm_unlink(shm_tally_real);

    if(get_is_open_other_authentication())
    {
        //gs_debug("open other authentication, disable pam_tally.");
        return 0;
    }
    if(!get_pam_tally(&deny, &unlock_time))
    {
        //gs_debug("get pam_tally configuration failed, disable pam_tally.");
        return 0;
    }

 /*   if(deny <= 0)
        deny = 3;
    if(unlock_time <= 0)
        unlock_time = 1800;
*/
    if( (fd = shm_open(shm_tally_real, O_RDWR | O_CREAT, FILE_MODE)) == -1)
    {
        printf("shm_open error: %s\n", strerror(errno));
        return -1;
    }

    ftruncate(fd, sizeof(pam_tally));

    if( (tally_ptr = mmap(NULL, sizeof(pam_tally),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED)
    {
        //gs_debug("mmap error: %s", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    tally_ptr->deny = deny;
    tally_ptr->unlock_time = unlock_time;
    tally_ptr->failed = 0;
    tally_ptr->lock_start_time = 0;

    return 1;
}

static
pam_tally* pam_tally_memory()
{
    int fd;
    pam_tally *tally_ptr;

    set_shm_tally_real();

    if( (fd = shm_open(shm_tally_real, O_RDWR, FILE_MODE)) == -1)
    {
        //gs_debug("shm_open error: %s", strerror(errno));
        return NULL;
    }

    if( (tally_ptr = mmap(NULL, sizeof(pam_tally),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED)
    {
        //gs_debug("mmap error: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    close(fd);
    return tally_ptr;
}

int pam_tally_is_enbled()
{
    int fd;

    set_shm_tally_real();

    if( (fd = shm_open(shm_tally_real, O_RDONLY, FILE_MODE)) == -1)
    {
        printf("shm_open error: %s\n", strerror(errno));
        return 0;
    }

    pam_tally *tally_ptr;

    if((tally_ptr = pam_tally_memory()) == NULL){
        close(fd);
        return -1;
    }

    if(tally_ptr->deny == 0 || tally_ptr->unlock_time == 0){
        close(fd);
	return 0;
    }
    close(fd);
    return 1;
}

int pam_tally_add_failed()
{
    pam_tally *tally_ptr;

    if((tally_ptr = pam_tally_memory()) == NULL)
        return -1;

    tally_ptr->failed++;

    //如果失败次数达到上限，开始计时
    if(tally_ptr->failed >= tally_ptr->deny)
        tally_ptr->lock_start_time = time(NULL);

    return 0;
}

int pam_tally_clear_failed()
{
    pam_tally *tally_ptr;

    if((tally_ptr = pam_tally_memory()) == NULL)
        return -1;

    tally_ptr->failed = 0;
    tally_ptr->lock_start_time = 0;
    return 0;
}

int pam_tally_failure_is_out()
{
    pam_tally *tally_ptr;

    if((tally_ptr = pam_tally_memory()) == NULL)
        return -1;

    return (tally_ptr->failed >= tally_ptr->deny ? 1 : 0);
}

int pam_tally_deny()
{
    pam_tally *tally_ptr;

    if((tally_ptr = pam_tally_memory()) == NULL)
        return -1;

    return tally_ptr->deny;
}

int pam_tally_failed_count()
{
    pam_tally *tally_ptr;

    if((tally_ptr = pam_tally_memory()) == NULL)
        return -1;

    return tally_ptr->failed;
}

int pam_tally_unlock_time()
{
    pam_tally *tally_ptr;

    if((tally_ptr = pam_tally_memory()) == NULL)
        return -1;

    printf("###########################  unlock time = %d\n", tally_ptr->unlock_time);
    return tally_ptr->unlock_time;
}

int pam_tally_is_canUnlock()
{
    pam_tally *tally_ptr;

    if((tally_ptr = pam_tally_memory()) == NULL)
        return -1;

    if(tally_ptr->failed >= tally_ptr->deny &&
       time(NULL) - tally_ptr->lock_start_time < tally_ptr->unlock_time)
        return 0;
    return 1;
}
