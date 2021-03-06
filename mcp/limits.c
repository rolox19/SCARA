#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <math.h>
#include <poll.h>

#include "error.h"
#include "file.h"
#include "gpio.h"
#include "task.h"
#include "queue.h"
#include "stepper.h"
#include "util.h"
#include "limits.h"
#include "core.h"

struct limits limits;
extern struct core core;

static void initLimitSwitch(struct limit *limit, uint8_t pin) {
	limit->pin = pin;
	gpio_export(limit->pin);
	gpio_direction(limit->pin, GPIO_DIR_IN);
	gpio_edge(limit->pin, GPIO_EDGE_FALLING);
}

static void limitsReadAll(void) {
	limits.limit[LIMITS_SHLDR_MIN].state = gpio_read(LIMITS_SHLDR_MIN_PIN);
	limits.limit[LIMITS_SHLDR_MAX].state = gpio_read(LIMITS_SHLDR_MAX_PIN);
	limits.limit[LIMITS_FOREARM_MIN].state = gpio_read(LIMITS_FOREARM_MIN_PIN);
	limits.limit[LIMITS_FOREARM_MAX].state = gpio_read(LIMITS_FOREARM_MAX_PIN);
}

void limitsInit(void) {
	printf("limitsThread initializing semaphores\n");
	sem_init(&limits.sem, 0, 0);
	sem_init(&limits.semRT, 0, 0);

	initLimitSwitch(&limits.limit[LIMITS_SHLDR_MIN], LIMITS_SHLDR_MIN_PIN);
	initLimitSwitch(&limits.limit[LIMITS_SHLDR_MAX], LIMITS_SHLDR_MAX_PIN);
	initLimitSwitch(&limits.limit[LIMITS_FOREARM_MIN], LIMITS_FOREARM_MIN_PIN);
	initLimitSwitch(&limits.limit[LIMITS_FOREARM_MAX], LIMITS_FOREARM_MAX_PIN);

	limitsReadAll();
}

static void limitSwitchCleanup(void) {
	gpio_unexport(limits.limit[LIMITS_SHLDR_MIN].pin);
	gpio_unexport(limits.limit[LIMITS_SHLDR_MAX].pin);
	gpio_unexport(limits.limit[LIMITS_FOREARM_MIN].pin);
	gpio_unexport(limits.limit[LIMITS_FOREARM_MAX].pin);
}

void *limitsThread(void *arg) {
	limitCmd command = LIMIT_STATUS;
	struct pollfd pfd[LIMIT_SWITCHES];
	int i, fd, rv;
	unsigned int timeout = 100; // ms

	// notify core thread we are ready
	sem_post(&limits.semRT);

	while(1) {
		if (!sem_trywait(&limits.sem)) {
			//printf("limits command = %d\n", limits.command);
			command = limits.command;
			switch (command) {
				case LIMIT_EXIT:
					limitSwitchCleanup();
					pthread_exit(0);
					break;
				case LIMIT_STATUS:
					limitsReadAll();
					break;
				default:
					fatal_error("unexpected command for limit thread\n");
					break;
			};

			// ack
			sem_post(&limits.semRT);
		}
		for (i = 0; i < LIMIT_SWITCHES; ++i) {
			fd = gpio_get_value_fd(limits.limit[i].pin);
			pfd[i].fd = fd;
			pfd[i].events = POLLPRI;
			gpio_read(limits.limit[i].pin);
			lseek(fd, 0, SEEK_SET);
		}
		rv = poll(pfd, LIMIT_SWITCHES, timeout);
		if (rv < 0) {
			perror(NULL);
			warning("limit switch polling failed!\n");
		} else if (rv > 0) {
			//printf("limit switch triggered\n");
			core.command = CORE_LIMIT;
			// can sometimes bounce
			// limitsReadAll();
			// instead scan the poll fd array
			for (i = 0; i < LIMIT_SWITCHES; ++i) {
				//printf("%d\t0x%x\t0x%x\t0x%x\n", i, pfd[i].revents, POLLIN, pfd[i].revents & POLLIN);
				if (pfd[i].revents) { // & POLLIN) {
					limits.limit[i].state = 0;
				} else {
					limits.limit[i].state = 1;
				}
			}
			printf("limits - %d\t%d\t%d\t%d\n", limits.limit[0].state, limits.limit[1].state, limits.limit[2].state, limits.limit[3].state);
			sem_post(&core.sem);
			sem_wait(&core.semRT);
		}
	}
}
