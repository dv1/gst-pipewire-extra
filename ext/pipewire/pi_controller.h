#ifndef __GST_PIPEWIRE_PI_CONTROLLER_H__
#define __GST_PIPEWIRE_PI_CONTROLLER_H__


/* Implementation of a PI controller (= PID controller without differential component).
 *
 * Call pi_controller_init() on a PIController instance before using. Pass the Ki
 * and Kp (integral and proportial) factors here. These are the typical Ki/Kp factors
 * from PID controllers (without the Kd factor since we don't do the differential).
 *
 * Use pi_controller_reset() to reset the controller back to its initial state.
 *
 * Update steps are done using pi_controller_compute(), which produces a filtered
 * quantity out of the input. This function also accepts a time_scale argument,
 * which is a relative quantity. If the time between updates is not uniform, this
 * can be used to factor in this non-uniformity. One example would be to use
 * clock_gettime() to get timestamps for each update, calculate the delta between
 * update timestamps, and divide that delta by 1e9 such that 1.0 means 1 second.
 */


typedef struct
{
	double ki, kp;
	double integral;
}
PIController;


static inline void pi_controller_init(PIController *pi_controller, double ki, double kp)
{
	pi_controller->ki = ki;
	pi_controller->kp = kp;
	pi_controller->integral = 0.0;
}


static inline void pi_controller_reset(PIController *pi_controller)
{
	pi_controller->integral = 0.0;
}


static inline double pi_controller_compute(PIController *pi_controller, double input, double time_scale)
{
	/* We factor the time_scale into the integral but not the proportial factor.
	 * That's because we need to _integrate_ the timespan covered by this update. */
	pi_controller->integral += input * time_scale;
	return pi_controller->integral * pi_controller->ki + input * pi_controller->kp;
}


#endif /* __GST_PIPEWIRE_PI_CONTROLLER_H__ */
