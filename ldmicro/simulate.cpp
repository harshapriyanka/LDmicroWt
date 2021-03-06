#include "simulate.h"
#include "maincontrols.h"
#include "ldmicro.h"
#include "miscutil.h"
#include "intcode.h"
#include <stdio.h>
#include<iostream>
#include <stdlib.h>
#include <limits.h>

// #include "intcode.h"
// #include "freezeLD.h"

static struct {
    char name[MAX_NAME_LEN];
    BOOL powered;
} SingleBitItems[MAX_IO];
static int SingleBitItemsCount;

static struct {
    char    name[MAX_NAME_LEN];
    SWORD   val;
    DWORD   usedFlags;
} Variables[MAX_IO];
static int VariablesCount;

static struct {
    char    name[MAX_NAME_LEN];
    SWORD   val;
} AdcShadows[MAX_IO];
static int AdcShadowsCount;

#define VAR_FLAG_TON  0x00000001
#define VAR_FLAG_TOF  0x00000002
#define VAR_FLAG_RTO  0x00000004
#define VAR_FLAG_CTU  0x00000008
#define VAR_FLAG_CTD  0x00000010
#define VAR_FLAG_CTC  0x00000020
#define VAR_FLAG_RES  0x00000040
#define VAR_FLAG_ANY  0x00000080

#define VAR_FLAG_OTHERWISE_FORGOTTEN  0x80000000


// Schematic-drawing code needs to know whether we're in simulation mode or
// note, as that changes how everything is drawn; also UI code, to disable
// editing during simulation.
BOOL InSimulationMode;

static BOOL UARTWindowInitialized = FALSE;

// Don't want to redraw the screen unless necessary; track whether a coil
// changed state or a timer output switched to see if anything could have
// changed (not just coil, as we show the intermediate steps too).
static BOOL NeedRedraw;
// Have to let the effects of a coil change in cycle k appear in cycle k+1,
// or set by the UI code to indicate that user manually changed an Xfoo
// input.
BOOL SimulateRedrawAfterNextCycle;

// Don't want to set a timer every 100 us to simulate a 100 us cycle
// time...but we can cycle multiple times per timer interrupt and it will
// be almost as good, as long as everything runs fast.
static int CyclesPerTimerTick;

// Program counter as we evaluate the intermediate code.
static int IntPc;

// A window to allow simulation with the UART stuff (insert keystrokes into
// the program, view the output, like a terminal window).
/*static QDialog* UartSimulationWindow;
static QPlainTextEdit* UartSimulationTextControl;*/
static LONG_PTR PrevTextProc;

static int QueuedUartCharacter = -1;
static int SimulateUartTxCountdown = 0;

// Flags to verify textchange on UART terminal is due to external events
// and not due to program changes
static BOOL InternalChange = FALSE;
static BYTE ChangeChar;

static void AppendToUartSimulationTextControl(BYTE b);

static void SimulateIntCode(void);
static char *MarkUsedVariable(char *name, DWORD flag);

//-----------------------------------------------------------------------------
// Query the state of a single-bit element (relay, digital in, digital out).
// Looks in the SingleBitItems list; if an item is not present then it is
// FALSE by default.
//-----------------------------------------------------------------------------
static BOOL SingleBitOn(char *name)
{
    int i;
    for(i = 0; i < SingleBitItemsCount; i++) {
        if(strcmp(SingleBitItems[i].name, name)==0) {
            return SingleBitItems[i].powered;
        }
    }
    return FALSE;
}

//-----------------------------------------------------------------------------
// Set the state of a single-bit item. Adds it to the list if it is not there
// already.
//-----------------------------------------------------------------------------
static void SetSingleBit(char *name, BOOL state)
{
    int i;
    for(i = 0; i < SingleBitItemsCount; i++) {
        // printf("%s,%d\n", name, SingleBitOn(name));
        if(strcmp(SingleBitItems[i].name, name)==0) {
            SingleBitItems[i].powered = state;
            return;
        }
    }
    if(i < MAX_IO) {
        strcpy(SingleBitItems[i].name, name);
        SingleBitItems[i].powered = state;
        SingleBitItemsCount++;
    }
}

//-----------------------------------------------------------------------------
// Count a timer up (i.e. increment its associated count by 1). Must already
// exist in the table.
//-----------------------------------------------------------------------------
static void IncrementVariable(char *name)
{
    int i;
    for(i = 0; i < VariablesCount; i++) {
        if(strcmp(Variables[i].name, name)==0) {
            (Variables[i].val)++;
            return;
        }
    }
    oops();
}

//-----------------------------------------------------------------------------
// Set a variable to a value.
//-----------------------------------------------------------------------------
static void SetSimulationVariable(char *name, SWORD val)
{
    int i;
    for(i = 0; i < VariablesCount; i++) {
        if(strcmp(Variables[i].name, name)==0) {
            Variables[i].val = val;
            return;
        }
    }
    MarkUsedVariable(name, VAR_FLAG_OTHERWISE_FORGOTTEN);
    SetSimulationVariable(name, val);
}

//-----------------------------------------------------------------------------
// Read a variable's value.
//-----------------------------------------------------------------------------
SWORD GetSimulationVariable(char *name)
{
    int i;
    for(i = 0; i < VariablesCount; i++) {
        if(strcmp(Variables[i].name, name)==0) {
            return Variables[i].val;
        }
    }
    MarkUsedVariable(name, VAR_FLAG_OTHERWISE_FORGOTTEN);
    return GetSimulationVariable(name);
}

//-----------------------------------------------------------------------------
// Set the shadow copy of a variable associated with a READ ADC operation. This
// will get committed to the real copy when the rung-in condition to the
// READ ADC is true.
//-----------------------------------------------------------------------------
void SetAdcShadow(char *name, SWORD val)
{
    int i;
    for(i = 0; i < AdcShadowsCount; i++) {
        if(strcmp(AdcShadows[i].name, name)==0) {
            AdcShadows[i].val = val;
            return;
        }
    }
    strcpy(AdcShadows[i].name, name);
    AdcShadows[i].val = val;
    AdcShadowsCount++;
}

//-----------------------------------------------------------------------------
// Return the shadow value of a variable associated with a READ ADC. This is
// what gets copied into the real variable when an ADC read is simulated.
//-----------------------------------------------------------------------------
SWORD GetAdcShadow(char *name)
{
    int i;
    for(i = 0; i < AdcShadowsCount; i++) {
        if(strcmp(AdcShadows[i].name, name)==0) {
            return AdcShadows[i].val;
        }
    }
    return 0;
}

//-----------------------------------------------------------------------------
// Mark how a variable is used; a series of flags that we can OR together,
// then we can check to make sure that only valid combinations have been used
// (e.g. just a TON, an RTO with its reset, etc.). Returns NULL for success,
// else an error string.
//-----------------------------------------------------------------------------
static char *MarkUsedVariable(char *name, DWORD flag)
{
    int i;
    for(i = 0; i < VariablesCount; i++) {
        if(strcmp(Variables[i].name, name)==0) {
            break;
        }
    }
    if(i >= MAX_IO) return "";

    if(i == VariablesCount) {
        strcpy(Variables[i].name, name);
        Variables[i].usedFlags = 0;
        Variables[i].val = 0;
        VariablesCount++;
    }

    switch(flag) {
        case VAR_FLAG_TOF:
            if(Variables[i].usedFlags != 0) 
                return _("TOF: variable cannot be used elsewhere");
            break;

        case VAR_FLAG_TON:
            if(Variables[i].usedFlags != 0)
                return _("TON: variable cannot be used elsewhere");
            break;
        
        case VAR_FLAG_RTO:
            if(Variables[i].usedFlags & ~VAR_FLAG_RES)
                return _("RTO: variable can only be used for RES elsewhere");
            break;

        case VAR_FLAG_CTU:
        case VAR_FLAG_CTD:
        case VAR_FLAG_CTC:
        case VAR_FLAG_RES:
        case VAR_FLAG_ANY:
            break;

        case VAR_FLAG_OTHERWISE_FORGOTTEN:
            if(name[0] != '$') {
                Error(_("Variable '%s' not assigned to, e.g. with a "
                    "MOV statement, an ADD statement, etc.\r\n\r\n"
                    "This is probably a programming error; now it "
                    "will always be zero."), name);
            }
            break;

        default:
            oops();
    }

    Variables[i].usedFlags |= flag;
    return NULL;
}

//-----------------------------------------------------------------------------
// Check for duplicate uses of a single variable. For example, there should
// not be two TONs with the same name. On the other hand, it would be okay
// to have an RTO with the same name as its reset; in fact, verify that
// there must be a reset for each RTO.
//-----------------------------------------------------------------------------
static void MarkWithCheck(char *name, int flag)
{
    char *s = MarkUsedVariable(name, flag);
    if(s) {
        Error(_("Variable for '%s' incorrectly assigned: %s."), name, s);
    }
}

static void CheckVariableNamesCircuit(int which, void *elem)
{
    ElemLeaf *l = (ElemLeaf *)elem;
    char *name = NULL;
    DWORD flag;

    switch(which) {
        case ELEM_SERIES_SUBCKT: {
            int i;
            ElemSubcktSeries *s = (ElemSubcktSeries *)elem;
            for(i = 0; i < s->count; i++) {
                CheckVariableNamesCircuit(s->contents[i].which,
                    s->contents[i].d.any);
            }
            break;
        }

        case ELEM_PARALLEL_SUBCKT: {
            int i;
            ElemSubcktParallel *p = (ElemSubcktParallel *)elem;
            for(i = 0; i < p->count; i++) {
                CheckVariableNamesCircuit(p->contents[i].which,
                    p->contents[i].d.any);
            }
            break;
        }
        
        case ELEM_RTO:
        case ELEM_TOF:
        case ELEM_TON:
            if(which == ELEM_RTO)
                flag = VAR_FLAG_RTO;
            else if(which == ELEM_TOF)
                flag = VAR_FLAG_TOF;
            else if(which == ELEM_TON)
                flag = VAR_FLAG_TON;
            else oops();

            MarkWithCheck(l->d.timer.name, flag);

            break;

        case ELEM_CTU:
        case ELEM_CTD:
        case ELEM_CTC:
            if(which == ELEM_CTU)
                flag = VAR_FLAG_CTU;
            else if(which == ELEM_CTD)
                flag = VAR_FLAG_CTD;
            else if(which == ELEM_CTC)
                flag = VAR_FLAG_CTC;
            else oops();

            MarkWithCheck(l->d.counter.name, flag);

            break;

        case ELEM_RES:
            MarkWithCheck(l->d.reset.name, VAR_FLAG_RES);
            break;

        case ELEM_MOVE:
            MarkWithCheck(l->d.move.dest, VAR_FLAG_ANY);
            break;

        case ELEM_LOOK_UP_TABLE:
            MarkWithCheck(l->d.lookUpTable.dest, VAR_FLAG_ANY);
            break;

        case ELEM_PIECEWISE_LINEAR:
            MarkWithCheck(l->d.piecewiseLinear.dest, VAR_FLAG_ANY);
            break;

        case ELEM_READ_ADC:
            MarkWithCheck(l->d.readAdc.name, VAR_FLAG_ANY);
            break;

        case ELEM_ADD:
        case ELEM_SUB:
        case ELEM_MUL:
        case ELEM_DIV:
            MarkWithCheck(l->d.math.dest, VAR_FLAG_ANY);
            break;

        case ELEM_UART_RECV:
            MarkWithCheck(l->d.uart.name, VAR_FLAG_ANY);
            break;

        case ELEM_SHIFT_REGISTER: {
            int i;
            for(i = 1; i < l->d.shiftRegister.stages; i++) {
                char str[MAX_NAME_LEN+10];
                sprintf(str, "%s%d", l->d.shiftRegister.name, i);
                MarkWithCheck(str, VAR_FLAG_ANY);
            }
            break;
        }

        case ELEM_PERSIST:
        case ELEM_FORMATTED_STRING:
        case ELEM_SET_PWM:
        case ELEM_MASTER_RELAY:
        case ELEM_UART_SEND:
        case ELEM_PLACEHOLDER:
        case ELEM_COMMENT:
        case ELEM_OPEN:
        case ELEM_SHORT:
        case ELEM_COIL:
        case ELEM_CONTACTS:
        case ELEM_ONE_SHOT_RISING:
        case ELEM_ONE_SHOT_FALLING:
        case ELEM_EQU:
        case ELEM_NEQ:
        case ELEM_GRT:
        case ELEM_GEQ:
        case ELEM_LES:
        case ELEM_LEQ:
            break;

        default:
            oops();
    }
}

static void CheckVariableNames(void)
{
    int i;
    for(i = 0; i < Prog.numRungs; i++) {
        CheckVariableNamesCircuit(ELEM_SERIES_SUBCKT, Prog.rungs[i]);
    }
}

//-----------------------------------------------------------------------------
// The IF condition is true. Execute the body, up until the ELSE or the
// END IF, and then skip the ELSE if it is present. Called with PC on the
// IF, returns with PC on the END IF.
//-----------------------------------------------------------------------------
static void IfConditionTrue(void)
{
    IntPc++;
    // now PC is on the first statement of the IF body
    SimulateIntCode();
    // now PC is on the ELSE or the END IF
    if(IntCode[IntPc].op == INT_ELSE) {
        int nesting = 1;
        for(; ; IntPc++) {
            if(IntPc >= IntCodeLen) oops();

            if(IntCode[IntPc].op == INT_END_IF) {
                nesting--;
            } else if(INT_IF_GROUP(IntCode[IntPc].op)) {
                nesting++;
            }
            if(nesting == 0) break;
        }
    } else if(IntCode[IntPc].op == INT_END_IF) {
        return;
    } else {
        oops();
    }
}

//-----------------------------------------------------------------------------
// The IF condition is false. Skip the body, up until the ELSE or the END
// IF, and then execute the ELSE if it is present. Called with PC on the IF,
// returns with PC on the END IF.
//-----------------------------------------------------------------------------
static void IfConditionFalse(void)
{
    int nesting = 0;
    for(; ; IntPc++) {
        if(IntPc >= IntCodeLen) oops();

        if(IntCode[IntPc].op == INT_END_IF) {
            nesting--;
        } else if(INT_IF_GROUP(IntCode[IntPc].op)) {
            nesting++;
        } else if(IntCode[IntPc].op == INT_ELSE && nesting == 1) {
            break;
        }
        if(nesting == 0) break;
    }

    // now PC is on the ELSE or the END IF
    if(IntCode[IntPc].op == INT_ELSE) {
        IntPc++;
        SimulateIntCode();
    } else if(IntCode[IntPc].op == INT_END_IF) {
        return;
    } else {
        oops();
    }
}

//-----------------------------------------------------------------------------
// Evaluate a circuit, calling ourselves recursively to evaluate if/else
// constructs. Updates the on/off state of all the leaf elements in our
// internal tables. Returns when it reaches an end if or an else construct,
// or at the end of the program.
//-----------------------------------------------------------------------------
static void SimulateIntCode(void)
{
    for(; IntPc < IntCodeLen; IntPc++) {
        IntOp *a = &IntCode[IntPc];
        switch(a->op) {
            case INT_SIMULATE_NODE_STATE:
                if(*(a->poweredAfter) != SingleBitOn(a->name1))
                    NeedRedraw = TRUE;
                *(a->poweredAfter) = SingleBitOn(a->name1);
                break;

            case INT_SET_BIT:
                SetSingleBit(a->name1, TRUE);
                break;

            case INT_CLEAR_BIT:
                SetSingleBit(a->name1, FALSE);
                break;

            case INT_COPY_BIT_TO_BIT:
                SetSingleBit(a->name1, SingleBitOn(a->name2));
                break;

            case INT_SET_VARIABLE_TO_LITERAL:
                if(GetSimulationVariable(a->name1) !=
                    a->literal && a->name1[0] != '$')
                {
                    NeedRedraw = TRUE;
                }
                SetSimulationVariable(a->name1, a->literal);
                break;

            case INT_SET_VARIABLE_TO_VARIABLE:
                if(GetSimulationVariable(a->name1) != 
                    GetSimulationVariable(a->name2))
                {
                    NeedRedraw = TRUE;
                }
                SetSimulationVariable(a->name1,
                    GetSimulationVariable(a->name2));
                break;

            case INT_INCREMENT_VARIABLE:
                IncrementVariable(a->name1);
                break;

            {
                SWORD v;
                case INT_SET_VARIABLE_ADD:
                    v = GetSimulationVariable(a->name2) +
                        GetSimulationVariable(a->name3);
                    goto math;
                case INT_SET_VARIABLE_SUBTRACT:
                    v = GetSimulationVariable(a->name2) -
                        GetSimulationVariable(a->name3);
                    goto math;
                case INT_SET_VARIABLE_MULTIPLY:
                    v = GetSimulationVariable(a->name2) *
                        GetSimulationVariable(a->name3);
                    goto math;
                case INT_SET_VARIABLE_DIVIDE:
                    if(GetSimulationVariable(a->name3) != 0) {
                        v = GetSimulationVariable(a->name2) /
                            GetSimulationVariable(a->name3);
                    } else {
                        v = 0;
                        Error(_("Division by zero; halting simulation"));
                        StopSimulation();
                    }
                    goto math;
math:
                    if(GetSimulationVariable(a->name1) != v) {
                        NeedRedraw = TRUE;
                        SetSimulationVariable(a->name1, v);
                    }
                    break;
            }

#define IF_BODY \
    { \
        IfConditionTrue(); \
    } else { \
        IfConditionFalse(); \
    }
            case INT_IF_BIT_SET:
                if(SingleBitOn(a->name1))
                    IF_BODY
                break;

            case INT_IF_BIT_CLEAR:
                if(!SingleBitOn(a->name1))
                    IF_BODY
                break;

            case INT_IF_VARIABLE_LES_LITERAL:
                if(GetSimulationVariable(a->name1) < a->literal)
                    IF_BODY
                break;

            case INT_IF_VARIABLE_EQUALS_VARIABLE:
                if(GetSimulationVariable(a->name1) ==
                    GetSimulationVariable(a->name2))
                    IF_BODY
                break;

            case INT_IF_VARIABLE_GRT_VARIABLE:
                if(GetSimulationVariable(a->name1) >
                    GetSimulationVariable(a->name2))
                    IF_BODY
                break;

            case INT_SET_PWM:
                // Dummy call will cause a warning if no one ever assigned
                // to that variable.
                (void)GetSimulationVariable(a->name1);
                break;

            // Don't try to simulate the EEPROM stuff: just hold the EEPROM
            // busy all the time, so that the program never does anything
            // with it.
            case INT_EEPROM_BUSY_CHECK:
                SetSingleBit(a->name1, TRUE);
                break;

            case INT_EEPROM_READ:
            case INT_EEPROM_WRITE:
                oops();
                break;

            case INT_READ_ADC:
                // Keep the shadow copies of the ADC variables because in
                // the real device they will not be updated until an actual
                // read is performed, which occurs only for a true rung-in
                // condition there.
                SetSimulationVariable(a->name1, GetAdcShadow(a->name1));
                break;

            case INT_UART_SEND:
                if(SingleBitOn(a->name2) && (SimulateUartTxCountdown == 0)) {
                    SimulateUartTxCountdown = 2;
                    AppendToUartSimulationTextControl(
                        (BYTE)GetSimulationVariable(a->name1));
                }
                if(SimulateUartTxCountdown == 0) {
                    SetSingleBit(a->name2, FALSE);
                } else {
                    SetSingleBit(a->name2, TRUE);
                }
                break;

            case INT_UART_RECV:
                if(QueuedUartCharacter >= 0) {
                    SetSingleBit(a->name2, TRUE);
                    SetSimulationVariable(a->name1, (SWORD)QueuedUartCharacter);
                    QueuedUartCharacter = -1;
                } else {
                    SetSingleBit(a->name2, FALSE);
                }
                break;

            case INT_END_IF:
            case INT_ELSE:
                return;

            case INT_COMMENT:
                break;
            
            default:
                oops();
                break;
        }
    }
}

//-----------------------------------------------------------------------------
// Called by the Windows timer that triggers cycles when we are running
// in real time.
//-----------------------------------------------------------------------------
BOOL PlcCycleTimer(BOOL kill = FALSE)
{
    for(int i = 0; i < CyclesPerTimerTick; i++) {
        SimulateOneCycle(FALSE);
    }

    return !kill;
}

//-----------------------------------------------------------------------------
// Simulate one cycle of the PLC. Update everything, and keep track of whether
// any outputs have changed. If so, force a screen refresh. If requested do
// a screen refresh regardless.
//-----------------------------------------------------------------------------
void SimulateOneCycle(BOOL forceRefresh)
{
    // When there is an error message up, the modal dialog makes its own
    // event loop, and there is risk that we would go recursive. So let
    // us fix that. (Note that there are no concurrency issues; we really
    // would get called recursively, not just reentrantly.)
    static BOOL Simulating = FALSE;

    if(Simulating) return;
    Simulating = TRUE;

    NeedRedraw = FALSE;

    if(SimulateUartTxCountdown > 0) {
        SimulateUartTxCountdown--;
    } else {
        SimulateUartTxCountdown = 0;
    }

    IntPc = 0;
    SimulateIntCode();

    if(NeedRedraw || SimulateRedrawAfterNextCycle || forceRefresh) {
        // InvalidateRect(DrawWindow, NULL, FALSE);
        // RefreshControlsToSettings();
    }

    SimulateRedrawAfterNextCycle = FALSE;
    if(NeedRedraw) SimulateRedrawAfterNextCycle = TRUE;

    Simulating = FALSE;
}

//-----------------------------------------------------------------------------
// Start the timer that we use to trigger PLC cycles in approximately real
// time. Independently of the given cycle time, just go at 40 Hz, since that
// is about as fast as anyone could follow by eye. Faster timers will just
// go instantly.
//-----------------------------------------------------------------------------
void StartSimulationTimer(void)
{
    /*int p = Prog.cycleTime/1000;
    if(p < 5) {
        SimulateTimer = SetTimer(DrawWindow, TIMER_SIMULATE, 10, SimulateTimer);
        CyclesPerTimerTick = 10000 / Prog.cycleTime;
    } else {
        SimulateTimer = SetTimer(DrawWindow, TIMER_SIMULATE, p, SimulateTimer);
        CyclesPerTimerTick = 1;
    }*/
}

//-----------------------------------------------------------------------------
// Clear out all the parameters relating to the previous simulation.
//-----------------------------------------------------------------------------
void ClearSimulationData(void)
{
    VariablesCount = 0;
    SingleBitItemsCount = 0;
    AdcShadowsCount = 0;
    QueuedUartCharacter = -1;
    SimulateUartTxCountdown = 0;

    CheckVariableNames();

    SimulateRedrawAfterNextCycle = TRUE;

    if(!GenerateIntermediateCode()) {
        ToggleSimulationMode();
        return;
    }

    SimulateOneCycle(TRUE);
}

//-----------------------------------------------------------------------------
// Provide a description for an item (Xcontacts, Ycoil, Rrelay, Ttimer,
// or other) in the I/O list.
//-----------------------------------------------------------------------------
void DescribeForIoList(char *name, char *out)
{
    switch(name[0]) {
        case 'R':
        case 'X':
        case 'Y':
            sprintf(out, "%d", SingleBitOn(name));
            break;

        case 'T': {
            double dtms = GetSimulationVariable(name) *
                (Prog.cycleTime / 1000.0);
            if(dtms < 1000) {
                sprintf(out, "%.2f ms", dtms);
            } else {
                sprintf(out, "%.3f s", dtms / 1000);
            }
            break;
        }
        default: {
            SWORD v = GetSimulationVariable(name);
            sprintf(out, "%hd (0x%04hx)", v, v);
            break;
        }
    }
}

//-----------------------------------------------------------------------------
// Toggle the state of a contact input; for simulation purposes, so that we
// can set the input state of the program.
//-----------------------------------------------------------------------------
void SimulationToggleContact(char *name)
{
    SetSingleBit(name, !SingleBitOn(name));
    // RefreshControlsToSettings();
    // ListView_RedrawItems(IoList, 0, Prog.io.count - 1);
}

static void UartSimulationTextProc(/*HWID hwid, UINT umsg, char *text, UINT uszbuf*/)
{
    /*char text = UartSimulationTextControl->toPlainText().toStdString().back();
    if(InternalChange)
    {
        if(text != ChangeChar)
        {
            QueuedUartCharacter = (BYTE)(text);
            InternalChange = FALSE;
        }
        return;
    }
    QueuedUartCharacter = (BYTE)(text);*/
}

//-----------------------------------------------------------------------------
// Pop up the UART simulation window; like a terminal window where the
// characters that you type go into UART RECV instruction and whatever
// the program puts into UART SEND shows up as text.
//-----------------------------------------------------------------------------
void ShowUartSimulationWindow(void)
{
    DWORD TerminalX = 200, TerminalY = 200, TerminalW = 300, TerminalH = 150;

/*    ThawDWORD(TerminalX);
    ThawDWORD(TerminalY);
    ThawDWORD(TerminalW);
    ThawDWORD(TerminalH);
*/
    if(TerminalW > 800) TerminalW = 100;
    if(TerminalH > 800) TerminalH = 100;

    // QRect r = QApplication::desktop()->screenGeometry();
 /*
    if(TerminalX >= (DWORD)(r.width() - 10)) TerminalX = 100;
    if(TerminalY >= (DWORD)(r.height() - 10)) TerminalY = 100;*/

    /*UartSimulationWindow = CreateWindowClient("UART Simulation (Terminal)",
        TerminalX, TerminalY, TerminalW, TerminalH, MainWindow);
    UartSimulationWindow->setWindowFlags(Qt::Window
        | Qt::WindowMinimizeButtonHint |
        Qt::WindowStaysOnTopHint);
    UartSimulationWindow->setWindowFlags(Qt::Tool);

    UartSimulationTextControl = new QPlainTextEdit();
    UartSimulationTextControl->resize(TerminalW, TerminalH);

    QVBoxLayout* UartSimLayout = new QVBoxLayout(UartSimulationWindow);
    UartSimLayout->addWidget(UartSimulationTextControl);

    HFONT fixedFont = CreateFont(14, 0, 0, FW_REGULAR, FALSE, "Lucida Console");
    SetFont(UartSimulationTextControl, fixedFont);
    QObject::connect(UartSimulationTextControl,
        &QPlainTextEdit::modificationChanged, UartSimulationTextProc);
    UartSimulationWindow->raise();
    UartSimulationWindow->show();
    MainWindow->setFocus();*/
    UARTWindowInitialized = TRUE;
}

//-----------------------------------------------------------------------------
// Get rid of the UART simulation terminal-type window.
//-----------------------------------------------------------------------------
void DestroyUartSimulationWindow(void)
{
    // Try not to destroy the window if it is already destroyed; that is
    // not for the sake of the window, but so that we don't trash the
    // stored position.

    if(UARTWindowInitialized)
    {
        UARTWindowInitialized = FALSE;
        
        DWORD TerminalX, TerminalY, TerminalW, TerminalH;
        // QRect r;

        // r = UartSimulationWindow->geometry();
/*        TerminalW = r.width();
        TerminalH = r.height();

        TerminalX = r.left();
        TerminalY = r.top();
*/
        /*FreezeDWORD(TerminalX);
        FreezeDWORD(TerminalY);
        FreezeDWORD(TerminalW);
        FreezeDWORD(TerminalH);*/

        /*delete UartSimulationTextControl;
        delete UartSimulationWindow;*/
    }
}

//-----------------------------------------------------------------------------
// Append a received character to the terminal buffer.
//-----------------------------------------------------------------------------
static void AppendToUartSimulationTextControl(BYTE b)
{
    if(UARTWindowInitialized)
    {
        char append[5];
    
        if((isalnum(b) || strchr("[]{};':\",.<>/?`~ !@#$%^&*()-=_+|", b) ||
            b == '\r' || b == '\n') && b != '\0')
        {
            append[0] = b;
            append[1] = '\0';
        } else {
            sprintf(append, "\\x%02x", b);
        }
    
    #define MAX_SCROLLBACK 256
        char buf[MAX_SCROLLBACK] = "\0";
        // strncpy(buf,
        //     UartSimulationTextControl->toPlainText().toStdString().c_str(),
        //     MAX_SCROLLBACK);
        int overBy = (strlen(buf) + strlen(append) + 1) - sizeof(buf);
        if(overBy > 0) {
            memmove(buf, buf + overBy, strlen(buf));
        }
        strcat(buf, append);
        InternalChange = TRUE;
        ChangeChar = b;
    
        // UartSimulationTextControl->setPlainText(buf);
    }
}

