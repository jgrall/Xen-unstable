
#ifndef __X86_TIME_H__
#define __X86_TIME_H__

extern int timer_ack;

extern void calibrate_tsc_bp(void);
extern void calibrate_tsc_ap(void);

#endif /* __X86_TIME_H__ */
