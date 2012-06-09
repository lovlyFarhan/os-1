#ifndef __DEBUG_HPP__
#define __DEBUG_HPP__

#include <sys/decls.h>

BEGIN_DECLS

extern const char DEBUG_KERNEL_INTERRUPTED_MESSAGE[];
extern const char DEBUG_USER_INTERRUPTED_MESSAGE[];

/**
 * \brief   Write a printf() style formatted string to debugging output
 *
 * \memberof Debug
 */
extern void printk (const char * format, ...);

END_DECLS

/**
 * \brief   Driver model to be implemented by anything wanting
 *          to provide a backend implemetation for printing out
 *          #printk() messages.
 *
 * To provide a backend for printk(), just write your subclass of DebugDriver.
 * Then declare a static global instance of it, and inside the constructor,
 * call Debug::RegisterDriver().
 *
 * For example:
 *
 * \code
 * class UartDebugDriver : public DebugDriver
 * {
 * public:
 *     UartDebugDriver () {
 *         Debug::RegisterDriver(this);
 *     }
 *
 *     virtual ~UartDebugDriver () {}
 *
 *     virtual void Init () {
 *         // setup hardware...
 *     }
 *
 *     virtual void PrintMessage (const char message[]) {
 *         // sent bytes of message out to hardware
 *     }
 * };
 *
 * static UartDebugDriver driverInstance;
 * \endcode
 */
class DebugDriver
{
public:
    /**
     * Anchor virtual method
     */
    virtual ~DebugDriver () {}

    /**
     * Perform any hardware initialization required
     */
    virtual void Init () = 0;

    /**
     * Send message payload out to hardware. Must not sleep, and must
     * be able to function with interrupts disabled.
     */
    virtual void PrintMessage (const char message[]) = 0;
};

/**
 * Registration and dispatch facilities for debug-drivers
 */
class Debug
{
public:
    /**
     * Call used by a concrete peripheral #DebugDriver
     * subclass to notify the debugging core that it exists.
     */
    static void RegisterDriver (DebugDriver * driver);

private:
    /**
     * Call used by external clients to request a string message
     * be routed to whatever debug-output backend is installed.
     */
    static void PrintMessage (const char message[]);

    friend void printk (const char *, ...);
};

#endif /* __DEBUG_HPP__ */
