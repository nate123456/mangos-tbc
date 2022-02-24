using Microsoft.AspNetCore.Mvc;
using Playerbot.Api.Abstractions;
using Playerbot.Api.Models;

namespace Playerbot.Api.Controllers;

[ApiController]
[Route("[controller]")]
public class ScriptsController : ControllerBase
{
    private readonly IPlayerbotRepository _repository;

    public ScriptsController(IPlayerbotRepository repository)
    {
        _repository = repository;
    }

    [HttpGet("{token}/")]
    public async Task<PlayerbotToken?> Token(string token)
    {
        var tokenEntry = await _repository.GetTokenFromStringAsync(token);

        return tokenEntry;
    }

    [HttpGet("{id:int}/")]
    public async Task<IEnumerable<PlayerbotScript>> Scripts(int id)
    {
        var scripts = await _repository.GetScriptsAsync(id);

        return scripts;
    }

    [HttpPost]
    public ActionResult SetScripts(PlayerbotScriptsDto dto)
    {
        _repository.SetScripts(dto.AccountId, dto.Scripts, dto.IsComplete);

        return Accepted();
    }

    [HttpPost("delete/")]
    public async Task<ActionResult> DeleteScript(PlayerbotScriptDeleteDto dto)
    {
        await _repository.DeleteScriptsAsync(dto.AccountId, dto.ScriptNames);

        return Accepted();
    }
}
