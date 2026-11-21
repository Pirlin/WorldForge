#include "WorldForgeGameplay.h"

int main()
{
    return worldforge::publish(worldforge::makeBehavior("Startup Beacon",worldforge::events::playStarted(),
        worldforge::effects::pulseBeacon(2.0f)));
}
