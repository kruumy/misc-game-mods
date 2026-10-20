using Terraria;
using Terraria.ModLoader;

namespace DashImmunity
{
    public class DashImmunityPlayer : ModPlayer
    {
        public override void PostUpdate()
        {
            // Another method, but can lead to too long of immunity
            
            if (Player.dashDelay == -1)
            {
                Player.immune = true;
            }

            if (Player.timeSinceLastDashStarted == 0)
            {
                OnPlayerDash();
            }
        }

        public void OnPlayerDash()
        {
            Player.immune = true;
            Player.immuneTime = 20;
        }
    }
}