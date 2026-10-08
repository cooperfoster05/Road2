#include "pch.h"
#include "RLCoach.h"
#include "bakkesmod/wrappers/includes.h"


BAKKESMOD_PLUGIN(RLCoach, "RL Coach: chess.com-style review for your training reps", plugin_version, PLUGINTYPE_FREEPLAY | PLUGINTYPE_CUSTOM_TRAINING)

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

void RLCoach::onLoad()
{
	_globalCvarManager = cvarManager;

	hudEnabled = std::make_shared<bool>(true);
	cvarManager->registerCvar("rlcoach_hud", "1", "Show the RL Coach test readout", true, true, 0, true, 1)
		.bindTo(hudEnabled);

	gameWrapper->RegisterDrawable([this](CanvasWrapper canvas) { RenderHud(canvas); });

	LOG("RL Coach loaded");
}

void RLCoach::onUnload()
{
	gameWrapper->UnregisterDrawables();
}

void RLCoach::RenderHud(CanvasWrapper canvas)
{
	if (!*hudEnabled) return;

	// Only run offline in free play or custom training
	if (!gameWrapper->IsInFreeplay() && !gameWrapper->IsInCustomTraining()) return;

	CarWrapper car = gameWrapper->GetLocalCar();
	if (car.IsNull()) return;

	// Speed in Unreal Units per second (supersonic is about 2200)
	float speed = car.GetVelocity().magnitude();
	bool supersonic = speed >= 2200.0f;

	// Boost as 0-100
	int boost = 0;
	BoostWrapper boostComp = car.GetBoostComponent();
	if (!boostComp.IsNull())
		boost = static_cast<int>(boostComp.GetCurrentBoostAmount() * 100.0f + 0.5f);

	bool onGround = car.IsOnGround();

	// Ball speed, if there is a ball
	float ballSpeed = 0.0f;
	ServerWrapper server = gameWrapper->GetGameEventAsServer();
	if (!server.IsNull())
	{
		BallWrapper ball = server.GetBall();
		if (!ball.IsNull())
			ballSpeed = ball.GetVelocity().magnitude();
	}

	// Draw a small panel in the top-left corner
	const int x = 40, y = 200, lineH = 22;
	canvas.SetColor(0, 0, 0, 150);
	canvas.SetPosition(Vector2{ x - 10, y - 10 });
	canvas.FillBox(Vector2{ 260, lineH * 5 + 14 });

	canvas.SetColor(255, 255, 255, 255);
	canvas.SetPosition(Vector2{ x, y });
	canvas.DrawString("RL COACH  (test readout)", 1.2f, 1.2f);

	if (supersonic) canvas.SetColor(255, 200, 0, 255);  // gold when supersonic
	else canvas.SetColor(255, 255, 255, 255);
	canvas.SetPosition(Vector2{ x, y + lineH * 1 });
	canvas.DrawString(std::format("Car speed: {:.0f} uu/s{}", speed, supersonic ? "  SUPERSONIC" : ""), 1.1f, 1.1f);

	canvas.SetColor(255, 255, 255, 255);
	canvas.SetPosition(Vector2{ x, y + lineH * 2 });
	canvas.DrawString(std::format("Boost: {}", boost), 1.1f, 1.1f);

	canvas.SetPosition(Vector2{ x, y + lineH * 3 });
	canvas.DrawString(std::format("On ground: {}", onGround ? "yes" : "no"), 1.1f, 1.1f);

	canvas.SetPosition(Vector2{ x, y + lineH * 4 });
	canvas.DrawString(std::format("Ball speed: {:.0f} uu/s", ballSpeed), 1.1f, 1.1f);
}
