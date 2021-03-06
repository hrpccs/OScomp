// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
/* Copyright (c) 2020 Facebook */

#include <argp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/mount.h>
#include <bpf/libbpf.h>
#include "exitcatch.h"
#include "exitcatch.skel.h"
#include "color.h"
#define NAMELIMIT 100
#define MAXLEN_PATH 100
#define LISTLIMT 500000
const char *KALLPATH = "/proc/kallsyms";
char logpath[MAXLEN_PATH] = "/var/log/crashlog";

char sysinfo_buffer[800];

typedef struct
{
	/*
		One node for symbol table
	*/
	unsigned long int address;
	char flag;
	char name[NAMELIMIT];
	//    symbolNode(unsigned long int _addr = 0, char _flag = 0, _name = ""):
} symbolNode;

typedef struct
{
	/*
		The whole table
	*/
	symbolNode nodeArray[LISTLIMT];
	int length;
} symbolList;
symbolList symList;

static int char2int(char c)
{
	if (c <= '9' && c >= '0')
		return c - '0';
	else
		return c - 'a' + 10;
}
static long int calIndex(int n)
{
	long int ans = 1;
	while (n--)
		ans *= 16;
	return ans;
}
static void initializeSym()
{
	FILE *fp = fopen(KALLPATH, "r");
	char *line = NULL;
	size_t len = 0;
	size_t readLength;
	symList.length = 0;
	while ((readLength = getline(&line, &len, fp)) != -1)
	{
		int mod = 0;
		for (int i = 0; i < readLength; i++)
		{
			// printf("%c",line[i]);
			if (line[i] == '\t' || line[i] == '\n')
				break;
			if (line[i] == ' ')
			{
				++mod;
				continue;
			}
			if (mod == 0)
			{
				if (i == 0)
					symList.nodeArray[symList.length].address = 0;
				symList.nodeArray[symList.length].address += calIndex(15 - i) * char2int(line[i]);
			}
			else if (mod == 1)
			{
				symList.nodeArray[symList.length].flag = line[i];
			}
			else
			{
				if (i == 19)
					memset(symList.nodeArray[symList.length].name, 0, sizeof(symList.nodeArray[symList.length].name));
				symList.nodeArray[symList.length].name[i - 19] = line[i];
			}
		}
		// printf("%#lx %c %s\n", symList.nodeArray[symList.length].address,symList.nodeArray[symList.length].flag,symList.nodeArray[symList.length].name);
		++symList.length;
	}
}
static int quiSymbol(long int query)
{
	int left = 0, right = symList.length;
	while (left < right)
	{
		int mid = (left + right) / 2;
		if (symList.nodeArray[mid].address >= query)
			right = mid;
		else
			left = mid + 1;
	}
	left = left > 0 ? --left : left;
	return left;
}

static void initializeSysInfo()
{
	/*
		??????????????????
		https://blog.csdn.net/LvJzzZ/article/details/112029991
	*/
	FILE *fp;
	fp = popen("bash ../src/hardware.sh", "r");
	while (1)
	{
		memset(sysinfo_buffer, 0, sizeof(sysinfo_buffer));
		void *ptr = fgets(sysinfo_buffer, sizeof(sysinfo_buffer), fp);
		if (ptr == NULL)
			break;
		printf("%s", sysinfo_buffer);
	}
	pclose(fp);
}
struct exitcatch_bpf *skel;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG)
		return 0;
	return vfprintf(stderr, format, args);
}

static volatile bool exiting = false;

static void sig_handler(int sig)
{
	exiting = true;
}

static int handle_event(void *ctx, void *data, size_t data_sz)
{

	const struct event *e = data;
	struct tm *tm;
	char ts[64];
	time_t t;
	unsigned long stack = 0;
	unsigned long stacks[MAX_STACK_DEPTH] = {0};

	time(&t);
	tm = localtime(&t);
	strftime(ts, sizeof(ts), "%m-%d_%H:%M:%S", tm);

	char filename[NAMELIMIT] = {0};
	strcpy(filename, e->comm);
	strcat(filename, "_");
	strcat(filename, ts);
	strcat(filename, ".log");
	printf("%s\n", filename);
	chdir(logpath);
	FILE *fp = fopen(filename, "w");
	int ret = bpf_map__lookup_elem(
		skel->maps.map_stack_traces,
		&e->stack_id,
		sizeof(unsigned int),
		&stacks,
		VALUESIZE,
		0);

	if (ret != 0)
	{
		printf("Error finding stack trace\n");
		return 0;
	}
	fprintf(fp, "%-14s %-16s %-7s %-7s %-7s %-9s %s\n",
			"TIME", "COMM", "TID", "PID", "PPID", "EXIT CODE", "SIGNALS");
	fprintf(fp, "%-14s %-16s %-7d %-7d %-7d %-9d %d\n",
			ts, e->comm, e->tid, e->pid, e->ppid, e->exit_code, e->sig);
	fprintf(fp, "Stack Trace:\n");
	printf(HEAD "%-14s %-16s %-7s %-7s %-7s %-9s %s" NONE "\n",
		   "TIME", "COMM", "TID", "PID", "PPID", "EXIT CODE", "SIGNALS");
	printf(LIGHT_GREEN "%-14s" YELLOW " %-16s" NONE " %-7d %-7d %-7d" RED " %-9d %d\n" NONE,
		   ts, e->comm, e->tid, e->pid, e->ppid, e->exit_code, e->sig);
	printf(YELLOW "Stack Trace:\n" NONE);
	for (int i = 0; i < MAX_STACK_DEPTH; i++)
	{
		stack = stacks[i];
		if (stack == 0)
		{
			break;
		}
		int index = quiSymbol(stack);
		fprintf(fp, "    %#lx %s+%#lx\n", stack, symList.nodeArray[index].name, stack - symList.nodeArray[index].address);
		printf("    %#lx %s+%#lx\n", stack, symList.nodeArray[index].name, stack - symList.nodeArray[index].address);
		// printf("    %#lx\n", stack);
	}
	// printf("%lx\n",e->count);

	// share lib
	printf(YELLOW "[Dependencies] Dynamic Libs:" NONE "\n");
	fprintf(fp, "[Dependencies] Dynamic Libs:\n");
	const struct mmap_struct *curr;
	for (int i = 0; i < e->count; i++)
	{
		curr = &(e->mmap[i]);
		fprintf(fp, "%08lx-%08lx %c%c%c%c %08llx %02x:%02x %lu ",
				curr->start,
				curr->end,
				curr->flags & VM_READ ? 'r' : '-',
				curr->flags & VM_WRITE ? 'w' : '-',
				curr->flags & VM_EXEC ? 'x' : '-',
				curr->flags & VM_MAYSHARE ? 's' : 'p',
				curr->pgoff,
				MAJOR(curr->dev), MINOR(curr->dev), curr->ino);
		printf("%08lx-%08lx %c%c%c%c %08llx %02x:%02x %lu ",
			   curr->start,
			   curr->end,
			   curr->flags & VM_READ ? 'r' : '-',
			   curr->flags & VM_WRITE ? 'w' : '-',
			   curr->flags & VM_EXEC ? 'x' : '-',
			   curr->flags & VM_MAYSHARE ? 's' : 'p',
			   curr->pgoff,
			   MAJOR(curr->dev), MINOR(curr->dev), curr->ino);
		for (int i = 0; i < MAX_LEVEL; i++)
		{
			if (curr->name[i][0] == '\0' || curr->name[i][0] == '/')
			{
				continue;
			}
			fprintf(fp, "/%s", curr->name[i]);
			printf("/%s", curr->name[i]);
		}

		// printf("Hello World");
		fprintf(fp, "\n");
		printf("\n");
	}
	fprintf(fp, "\n");
	fclose(fp);
	return 0;
}

int main(int argc, char **argv)
{
	initializeSysInfo();
	initializeSym();

	printf(YELLOW ">>>>>>>Finished initializing...\n" NONE);
	// sudo mount -t debugfs none /sys/kernel/debug
	struct ring_buffer *rb = NULL;
	int err;

	if (argc > 2)
	{
		return -1;
	}

	if (argc == 2)
		strcpy(logpath, argv[1]);

	printf(BLUE "Log will be saved in: %s" NONE "\n", logpath);
	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
	/* Set up libbpf errors and debug info callback */
	libbpf_set_print(libbpf_print_fn);

	/* Cleaner handling of Ctrl-C */
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	/* Load and verify BPF application */
	skel = exitcatch_bpf__open();
	if (!skel)
	{
		fprintf(stderr, "Failed to open and load BPF skeleton\n");
		return 1;
	}

	/* Load & verify BPF programs */
	err = exitcatch_bpf__load(skel);
	if (err)
	{
		fprintf(stderr, "Failed to load and verify BPF skeleton\n");
		goto cleanup;
	}

	/* Attach tracepoints */
	err = exitcatch_bpf__attach(skel);
	if (err)
	{
		fprintf(stderr, "Failed to attach BPF skeleton\n");
		goto cleanup;
	}

	/* Set up ring buffer polling */
	
	//
	mkdir(logpath, S_IRWXU);
	chdir(logpath);
	//
	rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_event, NULL, NULL);
		if (!rb)
		{
			err = -1;
			fprintf(stderr, "Failed to create ring buffer\n");
			goto cleanup;
		}
	/* Process events */

	while (!exiting)
	{
		err = ring_buffer__poll(rb, 100 /* timeout, ms */);
		/* Ctrl-C will cause -EINTR */
		if (err == -EINTR)
		{
			err = 0;
			break;
		}
		if (err < 0)
		{
			printf("Error polling perf buffer: %d\n", err);
			break;
		}
	}

cleanup:
	/* Clean up */
	ring_buffer__free(rb);
	exitcatch_bpf__destroy(skel);

	return err < 0 ? -err : 0;
}